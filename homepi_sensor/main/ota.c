// main/ota.c
#include "ota.h"
#include "led.h"
#include "nvs_manager.h"
#include "config.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include <string.h>

#include "mqtt_app.h"
#include "cJSON.h"
#include <time.h>

static const char *TAG = "ota";

// SHA256 du firmware téléchargé — vérifié avant apply
typedef struct {
    mbedtls_sha256_context ctx;
    uint8_t                digest[32];
    bool                   done;
} sha256_ctx_t;

static sha256_ctx_t s_sha = {0};

// Dernier échec OTA enregistré — stocké ici plutôt que publié directement,
// car au moment où ota_start() échoue, mqtt_handle est arrêté (voir
// mqtt_app.c::ota_launch_task, qui stoppe MQTT avant d'appeler ota_start()
// pour libérer la mémoire TLS nécessaire au téléchargement). Publier depuis
// ici échouerait silencieusement — esp_mqtt_client_publish() sur un client
// arrêté ne fait rien d'observable. La publication réelle se fait depuis
// mqtt_app.c, après le redémarrage du client MQTT.
typedef struct {
    bool     pending;
    char     reason[32];
    char     actual[65];
    char     expected[65];
} ota_failure_t;

static ota_failure_t s_last_failure = {0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            mbedtls_sha256_init(&s_sha.ctx);
            mbedtls_sha256_starts(&s_sha.ctx, 0);
            s_sha.done = false;
            break;
        case HTTP_EVENT_ON_DATA:
            if (!s_sha.done)
                mbedtls_sha256_update(&s_sha.ctx,
                    (const uint8_t*)evt->data, evt->data_len);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void sha256_to_hex(const uint8_t *digest, char *hex, size_t len)
{
    for (int i = 0; i < 32 && i*2+2 < (int)len; i++)
        snprintf(hex + i*2, 3, "%02x", digest[i]);
}

static void record_ota_failure(const char *reason, const char *actual, const char *expected)
{
    memset(&s_last_failure, 0, sizeof(s_last_failure));
    s_last_failure.pending = true;
    strlcpy(s_last_failure.reason, reason, sizeof(s_last_failure.reason));
    if (actual)   strlcpy(s_last_failure.actual,   actual,   sizeof(s_last_failure.actual));
    if (expected) strlcpy(s_last_failure.expected, expected, sizeof(s_last_failure.expected));
}

void ota_publish_last_failure(void)
{
    if (!s_last_failure.pending) return;
    if (!mqtt_handle) { s_last_failure.pending = false; return; }

    char device_id[64] = {0};
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    char topic[128];
    snprintf(topic, sizeof(topic), "iot/%s/status", device_id);

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "ota_failed");
    cJSON_AddStringToObject(p, "reason", s_last_failure.reason);
    if (s_last_failure.actual[0])
        cJSON_AddStringToObject(p, "sha256_actual", s_last_failure.actual);
    if (s_last_failure.expected[0])
        cJSON_AddStringToObject(p, "sha256_expected", s_last_failure.expected);
    cJSON_AddNumberToObject(p, "ts", (double)time(NULL));
    char *str = cJSON_PrintUnformatted(p);
    esp_mqtt_client_publish(mqtt_handle, topic, str, 0, 1, 0);
    ESP_LOGI(TAG, "Published OTA failure detail: %s", str);
    free(str);
    cJSON_Delete(p);

    s_last_failure.pending = false;
}

esp_err_t ota_start(const char *url, const char *expected_sha256)
{
    led_set(LED_OTA);
    ESP_LOGI(TAG, "OTA start: %s", url);

    static char root_ca[4096];
    nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca));

    esp_http_client_config_t http_cfg = {
        .url             = url,
        .cert_pem        = root_ca,
        .event_handler   = http_event_handler,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        record_ota_failure("begin_failed", esp_err_to_name(ret), NULL);
        led_set(LED_OPERATIONAL);
        return ret;
    }

    // Télécharger par chunks
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) continue;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        record_ota_failure("perform_failed", esp_err_to_name(ret), NULL);
        esp_https_ota_abort(ota_handle);
        led_set(LED_OPERATIONAL);
        return ret;
    }

    // Vérifier SHA256 avant de finaliser
    mbedtls_sha256_finish(&s_sha.ctx, s_sha.digest);
    mbedtls_sha256_free(&s_sha.ctx);
    s_sha.done = true;

    char actual_hex[65] = {0};
    sha256_to_hex(s_sha.digest, actual_hex, sizeof(actual_hex));
    ESP_LOGI(TAG, "SHA256 actual:   %s", actual_hex);
    ESP_LOGI(TAG, "SHA256 expected: %s", expected_sha256);

    if (expected_sha256 && strlen(expected_sha256) == 64) {
        if (strncmp(actual_hex, expected_sha256, 64) != 0) {
            ESP_LOGE(TAG, "SHA256 MISMATCH — aborting OTA");
            record_ota_failure("sha256_mismatch", actual_hex, expected_sha256);
            esp_https_ota_abort(ota_handle);
            led_set(LED_ERROR);
            return ESP_ERR_OTA_VALIDATE_FAILED;
        }
        ESP_LOGI(TAG, "SHA256 verified ✓");
    }

    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        record_ota_failure("finish_failed", esp_err_to_name(ret), NULL);
        led_set(LED_OPERATIONAL);
        return ret;
    }

    ESP_LOGI(TAG, "OTA success — rebooting in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK; // jamais atteint
}