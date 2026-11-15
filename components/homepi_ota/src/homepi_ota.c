// components/homepi_ota/src/homepi_ota.c
// Signed OTA: manifest signature, device checks, download, image checks, install. See homepi_ota.h.
#include "homepi_ota.h"
#include "nvs_manager.h"

#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota";

#define NVS_KEY_ROOT_CA "root_ca"   // same key as homepi_identity
#define CA_MAX          4096

extern const char ota_signing_pub_pem_start[] asm("_binary_ota_signing_pub_pem_start");
extern const char ota_signing_pub_pem_end[]   asm("_binary_ota_signing_pub_pem_end");

__attribute__((weak)) void hota_on_event(hota_event_t event) { (void)event; }

// ── Last failure (published by the application once MQTT is back) ──

static struct {
    bool pending;
    char reason[40];
    char detail[96];
    char version[32];
} s_failure;

static esp_err_t refuse(esp_err_t err, const char *reason, const char *detail, const char *version)
{
    memset(&s_failure, 0, sizeof(s_failure));
    s_failure.pending = true;
    strlcpy(s_failure.reason, reason, sizeof(s_failure.reason));
    if (detail)  strlcpy(s_failure.detail, detail, sizeof(s_failure.detail));
    if (version) strlcpy(s_failure.version, version, sizeof(s_failure.version));
    ESP_LOGE(TAG, "OTA refused: %s%s%s", reason, detail ? ": " : "", detail ? detail : "");
    hota_on_event(HOTA_EVENT_FAILED);
    return err;
}

bool hota_take_last_failure(char *json, size_t len)
{
    if (!s_failure.pending) return false;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "ota_failed");
    cJSON_AddStringToObject(p, "reason", s_failure.reason);
    if (s_failure.detail[0])  cJSON_AddStringToObject(p, "detail", s_failure.detail);
    if (s_failure.version[0]) cJSON_AddStringToObject(p, "version", s_failure.version);
    cJSON_AddStringToObject(p, "running", esp_app_get_description()->version);
    cJSON_AddNumberToObject(p, "ts", (double)time(NULL));
    bool ok = cJSON_PrintPreallocated(p, json, (int)len, false);
    cJSON_Delete(p);
    s_failure.pending = false;
    return ok;
}

// ── Manifest ─────────────────────────────────────────────────

static bool verify_signature(const char *manifest, const char *signature_b64)
{
    unsigned char sig[128], hash[32];
    size_t sig_len = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len, (const unsigned char *)signature_b64,
                              strlen(signature_b64)) != 0 || sig_len == 0)
        return false;
    if (mbedtls_sha256((const unsigned char *)manifest, strlen(manifest), hash, 0) != 0)
        return false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)ota_signing_pub_pem_start,
                                          ota_signing_pub_pem_end - ota_signing_pub_pem_start) == 0
              && mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA)
              && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

static bool copy_field(const cJSON *obj, const char *name, char *dst, size_t len)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, name));
    if (!v || strlen(v) >= len) return false;
    strlcpy(dst, v, len);
    return true;
}

#if CONFIG_IDF_TARGET_ESP32S3
#define CHIP_TARGET "esp32s3"
#elif CONFIG_IDF_TARGET_ESP32C3
#define CHIP_TARGET "esp32c3"
#else
#define CHIP_TARGET "esp32"
#endif

esp_err_t hota_check_manifest(const char *manifest, const char *signature_b64, hota_manifest_t *out)
{
    hota_manifest_t m = {0};
    if (!manifest || !signature_b64 || !manifest[0] || !signature_b64[0])
        return refuse(ESP_ERR_INVALID_ARG, "unsigned_command", NULL, NULL);
    if (strlen(manifest) >= HOTA_MANIFEST_MAX || strlen(signature_b64) >= HOTA_SIGNATURE_MAX)
        return refuse(ESP_ERR_INVALID_SIZE, "manifest_too_long", NULL, NULL);
    // The signature is checked first: nothing else in the command is trusted before it
    if (!verify_signature(manifest, signature_b64))
        return refuse(ESP_ERR_INVALID_CRC, "bad_signature", NULL, NULL);

    cJSON *j = cJSON_Parse(manifest);
    bool parsed = j && copy_field(j, "project", m.project, sizeof(m.project))
                  && copy_field(j, "chip_target", m.chip_target, sizeof(m.chip_target))
                  && copy_field(j, "version", m.version, sizeof(m.version))
                  && copy_field(j, "sha256", m.sha256, sizeof(m.sha256)) && strlen(m.sha256) == 64;
    cJSON_Delete(j);
    if (!parsed) return refuse(ESP_ERR_INVALID_ARG, "bad_manifest", NULL, NULL);

    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(m.project, running->project_name) != 0)
        return refuse(ESP_ERR_INVALID_ARG, "wrong_project", m.project, m.version);
    if (strcmp(m.chip_target, CHIP_TARGET) != 0)
        return refuse(ESP_ERR_INVALID_ARG, "wrong_chip", m.chip_target, m.version);
    bool comparable = false;
    int cmp = hota_version_cmp(m.version, running->version, &comparable);
    if (!comparable)
        return refuse(ESP_ERR_INVALID_VERSION, "version_not_comparable", running->version, m.version);
    if (cmp <= 0)
        return refuse(ESP_ERR_INVALID_VERSION, "not_newer", running->version, m.version);

    ESP_LOGI(TAG, "Signed manifest accepted: %s %s for %s", m.project, m.version, m.chip_target);
    if (out) *out = m;
    return ESP_OK;
}

// ── Download and install ─────────────────────────────────────

static mbedtls_sha256_context s_sha;

// The body of the HTTP response is the image: hashed as it arrives
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        mbedtls_sha256_init(&s_sha);
        mbedtls_sha256_starts(&s_sha, 0);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        mbedtls_sha256_update(&s_sha, (const unsigned char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

esp_err_t hota_start(const char *url, const char *manifest, const char *signature_b64)
{
    hota_manifest_t m;
    esp_err_t ret = hota_check_manifest(manifest, signature_b64, &m);
    if (ret != ESP_OK) return ret;
    if (!url || strncmp(url, "https://", 8) != 0)
        return refuse(ESP_ERR_INVALID_ARG, "bad_url", NULL, m.version);

    static char root_ca[CA_MAX];
    if (nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca)) != ESP_OK)
        return refuse(ESP_ERR_NOT_FOUND, "no_root_ca", NULL, m.version);

    hota_on_event(HOTA_EVENT_DOWNLOADING);
    ESP_LOGI(TAG, "Downloading %s %s", m.project, m.version);
    esp_http_client_config_t http = {
        .url = url,
        .cert_pem = root_ca,
        .event_handler = http_event,
        .keep_alive_enable = true,
        .timeout_ms = 30000,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    ret = esp_https_ota_begin(&cfg, &h);
    if (ret != ESP_OK) return refuse(ret, "begin_failed", esp_err_to_name(ret), m.version);

    // The image says what it is: it must be the version and the project the signed manifest names
    esp_app_desc_t desc;
    ret = esp_https_ota_get_img_desc(h, &desc);
    if (ret != ESP_OK) {
        esp_https_ota_abort(h);
        return refuse(ret, "no_image_descriptor", esp_err_to_name(ret), m.version);
    }
    if (strcmp(desc.version, m.version) != 0 || strcmp(desc.project_name, m.project) != 0) {
        esp_https_ota_abort(h);
        return refuse(ESP_ERR_INVALID_VERSION, "image_mismatch", desc.version, m.version);
    }

    do {
        ret = esp_https_ota_perform(h);
    } while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (ret != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        return refuse(ret != ESP_OK ? ret : ESP_FAIL, "download_failed", esp_err_to_name(ret), m.version);
    }

    unsigned char digest[32];
    char hex[65];
    mbedtls_sha256_finish(&s_sha, digest);
    mbedtls_sha256_free(&s_sha);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(hex, m.sha256) != 0) {
        esp_https_ota_abort(h);
        return refuse(ESP_ERR_OTA_VALIDATE_FAILED, "sha256_mismatch", hex, m.version);
    }
    ESP_LOGI(TAG, "Image matches the signed manifest (sha256 %.16s...)", hex);

    // Marks the new partition bootable; the image confirms itself on its first broker connection,
    // or the bootloader reverts to this one
    ret = esp_https_ota_finish(h);
    if (ret != ESP_OK) return refuse(ret, "finish_failed", esp_err_to_name(ret), m.version);
    ESP_LOGI(TAG, "%s installed: restarting", m.version);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
