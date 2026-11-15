// main/mqtt_app.c
#include "mqtt_app.h"
#include "config.h"
#include "nvs_manager.h"
#include "led.h"
#include "homepi_identity.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nilm_bridge.h"   // NILM : remplace "sensors.h" — ce device n'a
                           // pas de capteurs I2C, uniquement le pont Teensy
#include "homepi_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include <time.h>
#include <string.h>

static const char *TAG = "mqtt";

esp_mqtt_client_handle_t mqtt_handle = NULL;

static char s_device_id[64]       = {0};
static char s_topic_nilm[128]      = {0}; // NILM : topic dédié, PAS iot/.../sensors
                                           // (voir MQTT_TOPIC_NILM dans config.h)
static char s_topic_status[128]   = {0};
static char s_topic_commands[128] = {0};


// ── Handlers commandes MQTT ───────────────────────────────────

// OTA (homepi_ota, v2.4.1). The command carries the signed manifest; it is checked here, while MQTT is
// up, so that a refused command is reported at once without interrupting the broker session.
typedef struct {
    char url[256];
    char manifest[HOTA_MANIFEST_MAX];
    char signature[HOTA_SIGNATURE_MAX];
} ota_task_args_t;

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

static void ota_launch_task(void *arg)
{
    ota_task_args_t *args = (ota_task_args_t *)arg;
    // The download needs the heap the MQTT TLS session holds: no room for two TLS sessions
    ESP_LOGI(TAG, "OTA task: stopping MQTT to free TLS buffers");
    esp_mqtt_client_stop(mqtt_handle);
    vTaskDelay(pdMS_TO_TICKS(3000));
    hota_start(args->url, args->manifest, args->signature);   // restarts on success
    ESP_LOGE(TAG, "OTA failed: restarting MQTT");
    esp_mqtt_client_start(mqtt_handle);
    vTaskDelay(pdMS_TO_TICKS(3000));
    publish_ota_failure();
    free(args);
    vTaskDelete(NULL);
}

static void handle_ota(cJSON *cmd)
{
    const char *url       = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "url"));
    const char *manifest  = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "manifest"));
    const char *signature = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "signature"));
    if (!url || strlen(url) >= sizeof(((ota_task_args_t *)0)->url)) {
        ESP_LOGE(TAG, "OTA: missing or too long url");
        return;
    }
    if (hota_check_manifest(manifest, signature, NULL) != ESP_OK) {
        publish_ota_failure();
        return;
    }
    ota_task_args_t *args = calloc(1, sizeof(*args));
    if (!args) { ESP_LOGE(TAG, "OTA: out of memory"); return; }
    strlcpy(args->url, url, sizeof(args->url));
    strlcpy(args->manifest, manifest, sizeof(args->manifest));
    strlcpy(args->signature, signature, sizeof(args->signature));
    // Separate task: the MQTT event loop must not block, and only another task may stop the client
    xTaskCreate(ota_launch_task, "ota_launch", 8192, args, 5, NULL);
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
    // Redémarrer pour appliquer
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

    char *str = cJSON_PrintUnformatted(payload);
    // retain=1 : le broker mémorise le dernier statut
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

            // S'abonner aux commandes (QoS 1)
            int sub_ret = esp_mqtt_client_subscribe(
                mqtt_handle, s_topic_commands, 1);
            if (sub_ret < 0)
                ESP_LOGE(TAG, "Subscribe failed");
            else
                ESP_LOGI(TAG, "Subscribed to %s", s_topic_commands);

            // Publier statut online
            publish_status(true);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from broker");
            led_set(LED_ERROR);
            // No re-enrollment here (v2.4.0): an outage is not a revocation; ESP-MQTT reconnects
            // esp_mqtt_client gère la reconnexion automatiquement
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
    // Charger les credentials depuis NVS
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

    // Construire les topics
    // NILM : topic dédié nilm/<device_id>/telemetry, distinct de
    // iot/<device_id>/sensors utilisé par les capteurs environnementaux —
    // permet à Telegraf de router ces messages vers une mesure InfluxDB
    // séparée (nilm_readings) sans toucher au pipeline générique existant.
    snprintf(s_topic_nilm,     sizeof(s_topic_nilm),
             MQTT_TOPIC_NILM,     s_device_id);
    snprintf(s_topic_status,   sizeof(s_topic_status),
             MQTT_TOPIC_STATUS,   s_device_id);
    snprintf(s_topic_commands, sizeof(s_topic_commands),
             MQTT_TOPIC_COMMANDS, s_device_id);

    // LWT — publié automatiquement par le broker si déconnexion brutale
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
                // Valider le cert du broker avec le Root CA
                .certificate       = root_ca,
                .certificate_len   = strlen(root_ca) + 1,
                .skip_cert_common_name_check = false,
            },
        },
        .credentials = {
            // Client ID = device_id
            // use_identity_as_username=true côté Mosquitto →
            // CN du LDevID = username automatique
            .client_id = s_device_id,
            .authentication = {
                // LDevID = identité du device
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

// ── Publish NILM (appelé depuis main loop) ─────────────────────
// NILM : remplace mqtt_app_publish_sensors() du main.c générique — ce
// device n'a pas de capteurs I2C, uniquement le pont UART Teensy.

esp_err_t mqtt_app_publish_nilm(void)
{
    if (!mqtt_handle) return ESP_ERR_INVALID_STATE;
    return nilm_bridge_publish(mqtt_handle, s_device_id, s_topic_nilm);
}

// ── Graceful disconnect ───────────────────────────────────────

void mqtt_app_stop(void)
{
    if (!mqtt_handle) return;
    publish_status(false);          // offline explicite avant déconnexion
    vTaskDelay(pdMS_TO_TICKS(500)); // laisser le temps au publish
    esp_mqtt_client_stop(mqtt_handle);
    esp_mqtt_client_destroy(mqtt_handle);
    mqtt_handle = NULL;
}