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
#include "homepi_identity.h"
#include "esp_timer.h"
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

// A new image must reach the broker within this delay, or the device restarts and the bootloader
// goes back to the previous image (rollback enabled since v2.4.0)
#define OTA_CONFIRM_DEADLINE_US   (10LL * 60 * 1000000)
#define IDENTITY_CHECK_PERIOD_US  (24LL * 3600 * 1000000)

void hid_on_event(hid_event_t event)
{
    led_set(event == HID_EVENT_FAILED ? LED_ERROR : LED_ZERO_TOUCH);
}

static bool ota_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

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

    // The image is confirmed in mqtt_app.c on the first broker connection, not here
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

    // ── Identity (homepi_identity, v2.4.0) ────────────────────
    // Enrollment happens only without a usable LDevID (first boot, or expired after a long time
    // offline): a broker outage or a refused connection never re-enrolls any more.
    if (!hid_ldevid_usable()) {
        ESP_LOGI(TAG, "No usable LDevID: enrolling with the IDevID");
        if (hid_enroll() == ESP_OK) {
            ESP_LOGI(TAG, "LDevID stored: restarting");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        ESP_LOGE(TAG, "Enrollment failed: deep sleep 120 s");
        led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_deep_sleep(120 * 1000000ULL);
    }
    bool renewed = false;
    hid_renew_if_due(false, &renewed);
    // Tell the registry which LDevID this device holds. Refused while the device is not
    // pre-announced or approved: it keeps running, the operator decides.
    hid_register(NULL);

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
    int64_t last_identity_check = esp_timer_get_time();
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

        // LDevID renewal check once a day; the renewal itself happens when fewer than 30 days are left.
        // A renewed certificate is registered, then the device restarts to reconnect MQTT with it.
        if (esp_timer_get_time() - last_identity_check >= IDENTITY_CHECK_PERIOD_US) {
            last_identity_check = esp_timer_get_time();
            bool renewed_now = false;
            if (hid_renew_if_due(false, &renewed_now) == ESP_OK && renewed_now) {
                hid_register(NULL);
                esp_restart();
            }
        }

        // A new image that never reached the broker is reverted by the bootloader after this restart
        if (ota_pending_verify() && esp_timer_get_time() > OTA_CONFIRM_DEADLINE_US) {
            ESP_LOGE(TAG, "New firmware not confirmed within 10 minutes: restarting into the previous one");
            esp_restart();
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
