// main/main.c
#include "esp_sleep.h"
#include "nvs_manager.h"
#include "led.h"
#include "ntp_sync.h"
#include "homepi_identity.h"
#include "esp_timer.h"
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

// A new image must reach the broker within this delay, or the device restarts and the bootloader
// goes back to the previous image (rollback enabled since v2.4.0)
#define OTA_CONFIRM_DEADLINE_US   (10LL * 60 * 1000000)
#define IDENTITY_CHECK_PERIOD_US  (24LL * 3600 * 1000000)

// No I2C sensor on this device: what it tells the registry is fixed
static const hid_profile_t s_led_profile = { .hw_model = "led-strip", .capabilities_csv = "led" };
// status every 5 minutes: without it the registry cannot tell a silent controller (DeviceSilent)
#define HEARTBEAT_PERIOD_US  (5LL * 60 * 1000000)

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
    ESP_LOGI(TAG, "HomePi ESP32 %s boot", esp_app_get_description()->version);

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

    // ── LED strip: taken out of the critical connection path ──────
    // History: strip_control_init() was called HERE (before enrolment)
    // for consistency with the sensors_init() pattern of the sensor firmware.
    // Diagnosis (tcpdump + isolated test): led_strip_new_rmt_device() disturbs
    // network processing during the MQTT TLS handshake: the connection
    // went from a systematic 8-20 s stall to 1.2 s once the RMT init
    // was moved after MQTT starts. Exact cause still uncertain
    // (probably an RMT clock/interrupt interaction with Wi-Fi on the S3, absent
    // on the C3, which has no RMT channel); to document and dig into later,
    // but this reordering fixes the symptom reliably and cleanly:
    // the strip has no reason to exist before MQTT is up.

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
    hid_register(&s_led_profile);

    // ── MQTT mTLS ─────────────────────────────────────────────
    ESP_LOGI(TAG, "Enrolled — starting MQTT mTLS");
    ESP_LOGI(TAG, "Free heap before MQTT: %lu", (unsigned long)esp_get_free_heap_size());
    ESP_ERROR_CHECK(mqtt_app_start());
    led_set(LED_OPERATIONAL);

    // ── LED strip: init AFTER MQTT is established ─────────────
    // The TLS/MQTT socket is already up at this point; even if the RMT init has a
    // transient effect on interrupts, there is no handshake
    // in progress to suffer from it. The render task starts right after;
    // the strip shows its default layer (white, solid) immediately,
    // then the compositions received over MQTT apply normally.
    esp_err_t strip_ret = strip_control_init();
    if (strip_ret != ESP_OK) {
        ESP_LOGE(TAG, "Strip init failed (%s) — device continue quand meme",
                 esp_err_to_name(strip_ret));
    } else {
        strip_control_task_start();
    }

    // ── Main loop ─────────────────────────────────────
    int64_t last_identity_check = esp_timer_get_time();
    int64_t last_heartbeat = esp_timer_get_time();

    while (1) {
        // A queued OTA command runs here, on the main task (v2.4.2)
        mqtt_app_run_pending_ota();
        if (esp_timer_get_time() - last_heartbeat >= HEARTBEAT_PERIOD_US) {
            last_heartbeat = esp_timer_get_time();
            mqtt_app_publish_heartbeat();
        }

        // LDevID check once a day. The renewal itself runs at boot, before MQTT starts (no heap for
        // a second TLS session next to it): when fewer than 30 days are left, restart to renew.
        if (esp_timer_get_time() - last_identity_check >= IDENTITY_CHECK_PERIOD_US) {
            last_identity_check = esp_timer_get_time();
            if (hid_renewal_due() && hid_schedule_renewal() == ESP_OK) {
                ESP_LOGI(TAG, "LDevID renewal due: restarting to renew before MQTT");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }

        // A new image that never reached the broker is reverted by the bootloader after this restart
        if (ota_pending_verify() && esp_timer_get_time() > OTA_CONFIRM_DEADLINE_US) {
            ESP_LOGE(TAG, "New firmware not confirmed within 10 minutes: restarting into the previous one");
            esp_restart();
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