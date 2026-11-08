// main/main.c — variante NILM (device pont Teensy -> MQTT)
//
// Copie adaptée du main.c générique des capteurs HomePi. Réutilise TOUT
// le socle existant (WiFi, NTP, enrollment zero-touch, MQTT mTLS) sans
// aucune modification — seules les lignes marquées "NILM" changent :
//   - sensors_init()            -> nilm_bridge_init()
//   - mqtt_app_publish_sensors() -> nilm_bridge_publish(...)
//   - SENSOR_PUBLISH_INTERVAL (30s, pensé pour de la température) est
//     remplacé par un intervalle court dédié + publication immédiate sur
//     événement de charge (un démarrage/arrêt d'appareil ne doit pas
//     attendre jusqu'à 30s avant d'apparaître sur le dashboard).
#include "esp_sleep.h"
#include "nvs_manager.h"
#include "led.h"
#include "ntp_sync.h"
#include "enrollment.h"
#include "mqtt_app.h"
#include "nilm_bridge.h"      // NILM : remplace "sensors.h"
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

// NILM : intervalle de publication périodique court, distinct de
// SENSOR_PUBLISH_INTERVAL (config.h, 30s — conçu pour de la télémétrie
// environnementale lente, pas pour du suivi de charge électrique).
#define NILM_PUBLISH_INTERVAL_MS  1500

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

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT;
}

static void homepi_main_task(void *arg)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);

    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "HomePi NILM bridge v%s boot", esp_app_get_description()->version);

    // ── Init ───────────────────────────────────────────────────
    led_init();
    led_task_start();
    led_set(LED_BOOT);

    esp_event_loop_create_default();
    ESP_ERROR_CHECK(nvs_mgr_init());

    char device_id[64];
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    // ── WiFi ───────────────────────────────────────────────────
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

    // ── NILM bridge (UART Teensy) — AVANT enrollment ───────────
    // Même raisonnement que sensors_init() dans le main.c générique :
    // notify_api_enrolled() lit hw_model/capabilities depuis NVS pour
    // construire le payload d'enrollment, donc ce qui doit y figurer doit
    // être prêt avant. Contrairement aux capteurs I2C, nilm_bridge_init()
    // ne "découvre" rien de façon synchrone (la tâche UART tourne en
    // fond et se remplit progressivement) — non-bloquant par nature, donc
    // aucun risque de retarder l'enrollment.
    esp_err_t nilm_ret = nilm_bridge_init();
    if (nilm_ret != ESP_OK) {
        ESP_LOGW(TAG, "NILM bridge init failed (%s) — device pursue anyway",
                 esp_err_to_name(nilm_ret));
    }

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

    // ── Boucle principale ─────────────────────────────────────
    // NILM : publication périodique courte (1.5s, pas 30s) + publication
    // immédiate hors-cadence dès qu'un événement de charge est présent
    // dans la dernière trame reçue, pour que le dashboard voie les
    // démarrages/arrêts d'appareils sans latence artificielle.
    TickType_t last_publish = xTaskGetTickCount();
    time_t     last_renewal_check = 0;
    bool       last_event_state = false;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        bool has_event = nilm_bridge_has_pending_event();

        // Publication immédiate sur transition idle -> événement, en plus
        // du rythme périodique normal (évite de spammer le broker si
        // l'event_flag reste à 1 sur plusieurs cycles consécutifs — une
        // seule publication "hors-cadence" par transition, le reste suit
        // le rythme périodique habituel).
        bool event_rising_edge = has_event && !last_event_state;
        last_event_state = has_event;

        if (event_rising_edge ||
            (now - last_publish) >= pdMS_TO_TICKS(NILM_PUBLISH_INTERVAL_MS)) {
            mqtt_app_publish_nilm(); // wrapper dans mqtt_app.c, voir patch fourni
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

        vTaskDelay(pdMS_TO_TICKS(100)); // tick plus court que le main.c
                                        // générique (100ms vs 1000ms) pour
                                        // réagir vite à un event_flag
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreate(
        homepi_main_task,
        "homepi_main",
        32768,
        NULL,
        5,
        NULL
    );
}
