// main/main.c
#include "esp_sleep.h"
#include "nvs_manager.h"
#include "led.h"
#include "ntp_sync.h"
#include "enrollment.h"
#include "mqtt_app.h"
#include "sensors.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include <time.h>
#include "esp_system.h"

static const char *TAG = "main";
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
}

static esp_err_t wifi_connect(void)
{
    char ssid[64], pass[64];
    nvs_mgr_get_string(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
    nvs_mgr_get_string(NVS_KEY_WIFI_PASS, pass, sizeof(pass));

    ESP_LOGI(TAG, "WiFi → %s", ssid);
    wifi_events = xEventGroupCreate();

    esp_netif_init();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                wifi_event_handler, NULL);
    wifi_config_t wifi_cfg = {};
    strlcpy((char*)wifi_cfg.sta.ssid,     ssid, 32);
    strlcpy((char*)wifi_cfg.sta.password, pass, 64);
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_country_t country = {
        .cc = "JP",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    esp_wifi_set_country(&country);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    // esp_wifi_scan_start(NULL, true);
    // uint16_t n = 20;
    // wifi_ap_record_t aps[20];
    // esp_wifi_scan_get_ap_records(&n, aps);
    // for (int i=0; i<n; i++)
    //     ESP_LOGI(TAG, "AP: %s chan=%d rssi=%d", aps[i].ssid, aps[i].primary, aps[i].rssi);

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT;
}

static void homepi_main_task(void *arg)
{
    // esp_ota_mark_app_valid_cancel_rollback();
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    // 1=POWERON, 3=SW, 4=PANIC, 6=TASK_WDT, 7=INT_WDT, 9=BROWNOUT

    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "HomePi ESP32 v%s boot", esp_app_get_description()->version);

    // ── Init ───────────────────────────────────────────────────
    led_init();
    led_task_start();
    led_set(LED_BOOT);

    // ── Rollback cancel — OBLIGATOIRE ─────────────────────────
    // Si on arrive ici, le firmware a booté correctement.
    // Sans cet appel, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    // va rebooter sur l'ancien firmware au prochain watchdog.
    // esp_ota_mark_app_valid_cancel_rollback();
    // ESP_LOGI(TAG, "Firmware valid — rollback cancelled");

    esp_event_loop_create_default();
    ESP_ERROR_CHECK(nvs_mgr_init());

    char device_id[64];
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    // // ── WiFi ───────────────────────────────────────────────────
    led_set(LED_WIFI_CONNECTING);
    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — deep sleep 60s");
        led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_deep_sleep(60 * 1000000ULL);
    }

    // ── NTP ────────────────────────────────────────────────────
    if (ntp_sync_wait(NTP_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "NTP failed — reboot");
        led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ── Sensors I2C — AVANT enrollment ─────────────────────────
    // Doit s'exécuter avant enrollment_zero_touch()/enrollment_fallback() :
    // notify_api_enrolled() (appelée à l'intérieur de ces deux fonctions) lit
    // hw_model/capabilities depuis NVS pour construire le payload envoyé à
    // l'API. Si les capteurs ne sont pas encore découverts à ce moment-là,
    // ces champs NVS contiennent encore leur valeur par défaut du flash
    // (ex: "esp32c3" générique depuis generate_nvs.py, ou vide), jamais la
    // vraie combinaison de capteurs détectés — d'où hw_model incorrect ou
    // vide dans devices.db malgré un enrollment par ailleurs réussi.
    ESP_ERROR_CHECK(sensors_init());

    // ── Enrollment ────────────────────────────────────────────
    uint8_t enrolled = 0;
    nvs_mgr_get_u8(NVS_KEY_ENROLLED, &enrolled);
    if (!enrolled) {
        ESP_LOGI(TAG, "Not enrolled — trying zero-touch X5C");
        led_set(LED_ZERO_TOUCH);

        if (enrollment_zero_touch() != ESP_OK) {
            ESP_LOGW(TAG, "Zero-touch failed — fallback");
            led_set(LED_FALLBACK_WAIT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            enrollment_fallback();
        }
        ESP_LOGE(TAG, "Enrollment failed — deep sleep 120s");
        led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_deep_sleep(120 * 1000000ULL);
    }

    // ── MQTT mTLS ─────────────────────────────────────────────
    ESP_LOGI(TAG, "Enrolled — starting MQTT mTLS");
    ESP_ERROR_CHECK(mqtt_app_start());
    led_set(LED_OPERATIONAL);

    // Sensors déjà initialisés plus haut (avant enrollment) — pas de second
    // appel ici, sensors_init()/sensors_discover_and_init() ne doit être
    // exécuté qu'une seule fois par boot.

    // ── Boucle principale ─────────────────────────────────────
    TickType_t last_publish = xTaskGetTickCount();
    time_t     last_renewal_check = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_publish) >= pdMS_TO_TICKS(SENSOR_PUBLISH_INTERVAL)) {
            mqtt_app_publish_sensors();
            last_publish = now;
        }

        // Cert renewal check — une fois par jour entre 03:00 et 03:05
        time_t now_ts = time(NULL);
        struct tm t; localtime_r(&now_ts, &t);
        if (t.tm_hour == 3 && t.tm_min < 5 &&
            (now_ts - last_renewal_check) > 86400) {
            enrollment_check_renewal();
            last_renewal_check = now_ts;
        }


        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // Ne jamais atteindre ici
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreate(
        homepi_main_task,   // fonction
        "homepi_main",      // nom
        32768,              // stack en bytes — 32KB pour mbedtls + TLS
        NULL,               // paramètre
        5,                  // priorité
        NULL                // handle (pas besoin)
    );
}