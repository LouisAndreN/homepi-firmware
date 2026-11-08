// main/main.c
#include "esp_sleep.h"
#include "nvs_manager.h"
#include "led.h"
#include "ntp_sync.h"
#include "enrollment.h"
#include "mqtt_app.h"
#include "strip_control.h"
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
    ESP_LOGI(TAG, "HomePi ESP32 v%s boot", esp_app_get_description()->version);

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

    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power-save desactive (evite glitches RMT)");

    // ── NTP ────────────────────────────────────────────────────
    if (ntp_sync_wait(NTP_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "NTP failed — reboot");
        led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ── Strip LED — retiré du chemin critique de connexion ──────
    // Historique : strip_control_init() était appelé ICI (avant enrollment)
    // par cohérence avec le pattern sensors_init() du firmware capteur.
    // Diagnostic (tcpdump + test isolé) : led_strip_new_rmt_device() perturbe
    // le traitement réseau au moment du handshake TLS MQTT — connexion
    // passant de 8-20s de stall systématique à 1.2s une fois l'init RMT
    // déplacée après le démarrage de MQTT. Cause exacte encore incertaine
    // (probable interaction horloge/interruption RMT vs WiFi sur S3, absente
    // sur C3 qui n'a pas de canal RMT) — a documenter/creuser plus tard,
    // mais ce réordonnancement règle le symptôme de façon fiable et propre :
    // le strip n'a aucune raison d'exister avant que MQTT soit opérationnel.

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
    ESP_LOGI(TAG, "Free heap before MQTT: %lu", (unsigned long)esp_get_free_heap_size());
    ESP_ERROR_CHECK(mqtt_app_start());
    led_set(LED_OPERATIONAL);

    // ── Strip LED — init APRÈS que MQTT soit établi ─────────────
    // Le socket TLS/MQTT est déjà up à ce stade ; même si l'init RMT a un
    // effet transitoire sur les interruptions, il n'y a plus de handshake
    // en cours pour en souffrir. La tâche de rendu démarre juste après,
    // le strip affichera sa layer par défaut (blanc, solid) immédiatement,
    // puis les compositions reçues via MQTT s'appliqueront normalement.
    esp_err_t strip_ret = strip_control_init();
    if (strip_ret != ESP_OK) {
        ESP_LOGE(TAG, "Strip init failed (%s) — device continue quand meme",
                 esp_err_to_name(strip_ret));
    } else {
        strip_control_task_start();
    }

    // ── Boucle principale ─────────────────────────────────────
    time_t last_renewal_check = 0;

    while (1) {
        time_t now_ts = time(NULL);
        struct tm t; localtime_r(&now_ts, &t);
        if (t.tm_hour == 3 && t.tm_min < 5 &&
            (now_ts - last_renewal_check) > 86400) {
            enrollment_check_renewal();
            last_renewal_check = now_ts;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
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