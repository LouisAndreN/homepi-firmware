// main/mqtt_app.c
#include "mqtt_app.h"
#include "config.h"
#include "nvs_manager.h"
#include "led.h"
#include "homepi_identity.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nilm_bridge.h"   // NILM: replaces "sensors.h"; this device has
                           // no I2C sensors, only the Teensy bridge
#include "homepi_ota.h"
#include "teensy_ota.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include <time.h>
#include <string.h>

static const char *TAG = "mqtt";

esp_mqtt_client_handle_t mqtt_handle = NULL;

static char s_device_id[64]       = {0};
static char s_topic_nilm[128]      = {0}; // NILM: dedicated topic, NOT iot/.../sensors
                                           // (see MQTT_TOPIC_NILM in config.h)
static char s_topic_status[128]   = {0};
static char s_topic_commands[128] = {0};
static char s_topic_event[128]    = {0};   // v2.6.0: interruptions of the NILM data (teensy_update)


// ── MQTT command handlers ───────────────────────────────────

// OTA (homepi_ota). The command carries the signed manifest; it is checked here, while MQTT is up,
// so that a refused command is reported at once without interrupting the broker session.
// v2.4.2: the MQTT task only checks, queues and acknowledges. The main task runs the download
// (mqtt_app_run_pending_ota) in homepi_main, whose 32 KB stack exists from boot: nothing is allocated
// per command.
// v2.4.0/v2.4.1 created an 8 KB task here without checking the result: on the sensor with the least
// free heap (bh1750+bme688+sps30+sgp41) the command was logged, then nothing, and nothing reported.

void hota_on_event(hota_event_t event)
{
    led_set(event == HOTA_EVENT_DOWNLOADING ? LED_OTA : LED_OPERATIONAL);
}

static void publish_ota_failure(void)
{
    char json[256];
    if (hota_take_last_failure(json, sizeof(json)) && mqtt_handle)
        esp_mqtt_client_publish(mqtt_handle, s_topic_status, json, 0, 1, 0);
}

static void handle_ota(cJSON *cmd)
{
    const char *url       = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "url"));
    const char *manifest  = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "manifest"));
    const char *signature = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "signature"));
    if (hota_request(url, manifest, signature, NULL) != ESP_OK) {
        publish_ota_failure();
        return;
    }
    char ack[160];
    if (hota_received_json(ack, sizeof(ack)))
        esp_mqtt_client_publish(mqtt_handle, s_topic_status, ack, 0, 1, 0);
}

void mqtt_app_run_pending_ota(void)
{
    if (!hota_has_request() || !mqtt_handle) return;
    // Let the acknowledgment leave, then free the heap the MQTT TLS session holds: there is no room
    // for two TLS sessions
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "OTA: stopping MQTT to free TLS buffers");
    esp_mqtt_client_stop(mqtt_handle);
    vTaskDelay(pdMS_TO_TICKS(3000));
    hota_run_request();                     // restarts on success
    ESP_LOGE(TAG, "OTA failed: restarting MQTT (the reason is published once connected)");
    esp_mqtt_client_start(mqtt_handle);
}

// v2.6.0: remote update of the Teensy (teensy_ota.c). Checked here, run by the main task.
static void handle_teensy_ota(cJSON *cmd)
{
    teensy_ota_handle_command(cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "url")),
                              cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "manifest")),
                              cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "signature")));
}

void mqtt_app_publish_status(const char *json)
{
    if (mqtt_handle && json) esp_mqtt_client_publish(mqtt_handle, s_topic_status, json, 0, 1, 0);
}

void mqtt_app_publish_event(const char *kind, const char *phase, const char *ref, const char *detail)
{
    if (!mqtt_handle) return;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "kind", kind);
    cJSON_AddStringToObject(e, "phase", phase);
    cJSON_AddStringToObject(e, "ref", ref ? ref : "");
    cJSON_AddStringToObject(e, "detail", detail ? detail : "");
    cJSON_AddNumberToObject(e, "ts", (double)time(NULL));
    char *js = cJSON_PrintUnformatted(e);
    if (js) { esp_mqtt_client_publish(mqtt_handle, s_topic_event, js, 0, 1, 0); free(js); }
    cJSON_Delete(e);
}

static void handle_renew_cert(void)
{
    // Forced renewal (operator test). The renewal itself runs at the next boot, before MQTT starts:
    // a second TLS session next to the MQTT one does not fit in the heap.
    ESP_LOGI(TAG, "LDevID renewal requested via MQTT: renewing at the next boot");
    esp_err_t ret = hid_schedule_renewal();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot schedule the renewal: %s", esp_err_to_name(ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_update_wifi(cJSON *cmd)
{
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "password"));
    double effective_at = cJSON_GetNumberValue(cJSON_GetObjectItem(cmd, "effective_at"));

    if (!ssid || !pass) {
        ESP_LOGE(TAG, "update_wifi: missing fields");
        return;
    }

    time_t now = time(NULL);
    if (effective_at > 0 && (time_t)effective_at > now) {
        double delay_s = effective_at - (double)now;
        ESP_LOGI(TAG, "WiFi update scheduled in %.0fs → SSID: %s", delay_s, ssid);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(delay_s * 1000)));
    }

    ESP_LOGI(TAG, "Applying WiFi update: %s", ssid);
    nvs_mgr_set_string(NVS_KEY_WIFI_SSID, ssid);
    nvs_mgr_set_string(NVS_KEY_WIFI_PASS, pass);
    // Restart to apply
    esp_restart();
}

static void dispatch_command(const char *data, int len)
{
    cJSON *cmd = cJSON_ParseWithLength(data, len);
    if (!cmd) {
        ESP_LOGE(TAG, "Command: invalid JSON");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(cmd, "type");
    const char *type = cJSON_GetStringValue(type_item);

    if (!type) {
        ESP_LOGE(TAG, "Command: missing 'type' field");
        cJSON_Delete(cmd);
        return;
    }

    ESP_LOGI(TAG, "Command received: %s", type);

    if (strcmp(type, "ota") == 0) {
        handle_ota(cmd);
    } else if (strcmp(type, "teensy_ota") == 0) {
        handle_teensy_ota(cmd);
    } else if (strcmp(type, "renew_cert") == 0) {
        handle_renew_cert();
    } else if (strcmp(type, "update_wifi") == 0) {
        handle_update_wifi(cmd);
    } else {
        ESP_LOGW(TAG, "Unknown command type: %s", type);
    }

    cJSON_Delete(cmd);
}

// ── Publish statut online/offline ────────────────────────────

static void publish_status(bool online)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "online", online);
    cJSON_AddNumberToObject(payload, "ts", (double)time(NULL));
    // v2.4.2: heap margin, measured where it matters (the free heap now, the lowest since boot, the
    // largest block an allocation can get)
    cJSON_AddNumberToObject(payload, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(payload, "heap_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(payload, "heap_largest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    char *str = cJSON_PrintUnformatted(payload);
    // retain=1: the broker keeps the last status
    esp_mqtt_client_publish(mqtt_handle, s_topic_status,
                            str, 0, /*qos*/1, /*retain*/1);
    free(str);
    cJSON_Delete(payload);
}

// ── Event handler ─────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to broker");
            // A new image has now proven Wi-Fi, NTP, its LDevID and the broker: keep it
            {
                esp_ota_img_states_t st;
                if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
                    st == ESP_OTA_IMG_PENDING_VERIFY) {
                    esp_ota_mark_app_valid_cancel_rollback();
                    ESP_LOGI(TAG, "New firmware confirmed after the first broker connection");
                }
            }
            led_set(LED_OPERATIONAL);

            // Subscribe to commands (QoS 1)
            int sub_ret = esp_mqtt_client_subscribe(
                mqtt_handle, s_topic_commands, 1);
            if (sub_ret < 0)
                ESP_LOGE(TAG, "Subscribe failed");
            else
                ESP_LOGI(TAG, "Subscribed to %s", s_topic_commands);

            // Publier statut online
            publish_status(true);
            // An OTA refused or failed while offline is reported now that the session is up
            publish_ota_failure();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from broker");
            led_set(LED_ERROR);
            // No re-enrollment here (v2.4.0): an outage is not a revocation; ESP-MQTT reconnects
            // esp_mqtt_client handles reconnection automatically
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscription confirmed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            if (event->topic_len > 0) {
                ESP_LOGI(TAG, "Message on topic: %.*s",
                         event->topic_len, event->topic);
            }
            if (strncmp(event->topic, s_topic_commands,
                        event->topic_len) == 0) {
                dispatch_command(event->data, event->data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            if (event->error_handle->error_type ==
                    MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TLS or TCP error (broker down, or LDevID refused)");
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Message published, msg_id=%d", event->msg_id);
            break;

        default:
            ESP_LOGD(TAG, "MQTT event id=%d", event->event_id);
            break;
    }
}

// ── Init + start ──────────────────────────────────────────────

esp_err_t mqtt_app_start(void)
{
    // Load the credentials from NVS
    static char root_ca[4096]    = {0};
    static char ldevid_cert[3072]= {0};   // LDevID chain (v2.4.0)
    static char ldevid_key[2048] = {0};
    static char lwt_payload[128] = {0};

    ESP_ERROR_CHECK(nvs_mgr_get_string(NVS_KEY_DEVICE_ID,
                                       s_device_id, sizeof(s_device_id)));
    ESP_ERROR_CHECK(nvs_mgr_get_cert(NVS_KEY_ROOT_CA,
                                     root_ca, sizeof(root_ca)));
    ESP_ERROR_CHECK(nvs_mgr_get_active_ldevid(
        ldevid_cert, sizeof(ldevid_cert),
        ldevid_key,  sizeof(ldevid_key)));

    // Build the topics
    // NILM: dedicated topic nilm/<device_id>/telemetry, distinct from
    // iot/<device_id>/sensors used by the environmental sensors:
    // lets Telegraf route these messages to a separate InfluxDB measurement
    // (nilm_readings) without touching the existing generic pipeline.
    snprintf(s_topic_nilm,     sizeof(s_topic_nilm),
             MQTT_TOPIC_NILM,     s_device_id);
    snprintf(s_topic_status,   sizeof(s_topic_status),
             MQTT_TOPIC_STATUS,   s_device_id);
    snprintf(s_topic_commands, sizeof(s_topic_commands),
             MQTT_TOPIC_COMMANDS, s_device_id);
    snprintf(s_topic_event,    sizeof(s_topic_event),
             MQTT_TOPIC_EVENT,    s_device_id);

    // LWT: published automatically by the broker on an abrupt disconnection
    cJSON *lwt_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(lwt_json, "online", false);
    cJSON_AddNumberToObject(lwt_json, "ts", (double)time(NULL));
    char *lwt_str = cJSON_PrintUnformatted(lwt_json);
    strlcpy(lwt_payload, lwt_str, sizeof(lwt_payload));
    free(lwt_str);
    cJSON_Delete(lwt_json);

    ESP_LOGI(TAG, "Starting MQTT mTLS for device: %s", s_device_id);
    ESP_LOGI(TAG, "Broker: %s:%d", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname  = MQTT_BROKER_IP,
                .port      = MQTT_BROKER_PORT,
                .transport = MQTT_TRANSPORT_OVER_SSL,
            },
            .verification = {
                // Validate the broker certificate with the root CA
                .certificate       = root_ca,
                .certificate_len   = strlen(root_ca) + 1,
                .skip_cert_common_name_check = false,
            },
        },
        .credentials = {
            // Client ID = device_id
            // use_identity_as_username=true on the Mosquitto side ->
            // LDevID CN = automatic username
            .client_id = s_device_id,
            .authentication = {
                // LDevID = device identity
                .certificate     = ldevid_cert,
                .certificate_len = strlen(ldevid_cert) + 1,
                .key             = ldevid_key,
                .key_len         = strlen(ldevid_key) + 1,
            },
        },
        .session = {
            .keepalive = 60,
            .last_will = {
                .topic   = s_topic_status,
                .msg     = lwt_payload,
                .msg_len = strlen(lwt_payload),
                .qos     = 1,
                .retain  = 1,
            },
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms           = 10000,
        },
    };

    mqtt_handle = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_handle) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(mqtt_handle, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    return esp_mqtt_client_start(mqtt_handle);
}

// ── Publish NILM (called from the main loop) ─────────────────────
// NILM: replaces mqtt_app_publish_sensors() of the generic main.c; this
// device has no I2C sensors, only the Teensy UART bridge.

esp_err_t mqtt_app_publish_nilm(void)
{
    if (!mqtt_handle) return ESP_ERR_INVALID_STATE;
    return nilm_bridge_publish(mqtt_handle, s_device_id, s_topic_nilm);
}

// ── Graceful disconnect ───────────────────────────────────────

void mqtt_app_stop(void)
{
    if (!mqtt_handle) return;
    publish_status(false);          // explicit offline status before disconnecting
    vTaskDelay(pdMS_TO_TICKS(500)); // give the publish time to go out
    esp_mqtt_client_stop(mqtt_handle);
    esp_mqtt_client_destroy(mqtt_handle);
    mqtt_handle = NULL;
}