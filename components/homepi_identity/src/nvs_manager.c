// components/homepi_identity/src/nvs_manager.c (shared by the sensor, LED and NILM firmware since v2.4.0)
#include "nvs_manager.h"
#include "identity_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_mgr";

static nvs_handle_t h_config = 0;
static nvs_handle_t h_certs  = 0;

esp_err_t nvs_mgr_init(void)
{
    // Initialise the default NVS partition
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash erased and re-initialized");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialise the nvs_certs partition (never erased)
    ret = nvs_flash_init_partition("nvs_certs");
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // DO NOT erase nvs_certs: it holds the certificates
        ESP_LOGE(TAG, "nvs_certs partition corrupted! Cannot erase.");
        return ret;
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h_config));
    ESP_ERROR_CHECK(nvs_open_from_partition(
        "nvs_certs", NVS_NS_CERTS, NVS_READWRITE, &h_certs));

    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

// ── Config partition ──────────────────────────────────────────

esp_err_t nvs_mgr_get_string(const char *key, char *buf, size_t len)
{
    esp_err_t ret = nvs_get_str(h_config, key, buf, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ret;
}

esp_err_t nvs_mgr_set_string(const char *key, const char *val)
{
    esp_err_t ret = nvs_set_str(h_config, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h_config);
    return ret;
}

esp_err_t nvs_mgr_get_u8(const char *key, uint8_t *val)
{
    esp_err_t ret = nvs_get_u8(h_config, key, val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *val = 0;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ret;
}

esp_err_t nvs_mgr_set_u8(const char *key, uint8_t val)
{
    esp_err_t ret = nvs_set_u8(h_config, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h_config);
    return ret;
}


static void unescape_newlines(char *buf) {
    char *src = buf, *dst = buf;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\n';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ── Certs partition ───────────────────────────────────────────

esp_err_t nvs_mgr_get_cert(const char *key, char *buf, size_t len)
{
    esp_err_t ret = nvs_get_str(h_certs, key, buf, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        ESP_LOGW(TAG, "Cert not found in NVS: %s", key);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret == ESP_OK) unescape_newlines(buf);
    return ret;
}

esp_err_t nvs_mgr_set_cert(const char *key, const char *pem)
{
    esp_err_t ret = nvs_set_str(h_certs, key, pem);
    if (ret == ESP_OK) ret = nvs_commit(h_certs);
    return ret;
}

esp_err_t nvs_mgr_get_binary(const char *key, uint8_t *buf, size_t *len)
{
    esp_err_t ret = nvs_get_blob(h_certs, key, buf, len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Binary not found in NVS: %s", key);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ret;
}

esp_err_t nvs_mgr_set_binary(const char *key, const uint8_t *buf, size_t len)
{
    esp_err_t ret = nvs_set_blob(h_certs, key, buf, len);
    if (ret == ESP_OK) ret = nvs_commit(h_certs);
    return ret;
}

// ── LDevID A/B swap atomique ──────────────────────────────────
//
// Principle: always write to the INACTIVE slot, then switch.
// If the device restarts during the write, the old slot is still valid.
//
// Slot 0 (A) : NVS_KEY_LDEVID_CERT_A / NVS_KEY_LDEVID_KEY_A
// Slot 1 (B) : NVS_KEY_LDEVID_CERT_B / NVS_KEY_LDEVID_KEY_B

esp_err_t nvs_mgr_store_ldevid(const char *cert_pem, const char *key_pem)
{
    uint8_t current_slot = 0;
    nvs_mgr_get_u8(NVS_KEY_LDEVID_SLOT, &current_slot);
    uint8_t new_slot = current_slot ? 0 : 1;

    const char *cert_nvskey = new_slot
        ? NVS_KEY_LDEVID_CERT_B : NVS_KEY_LDEVID_CERT_A;
    const char *key_nvskey  = new_slot
        ? NVS_KEY_LDEVID_KEY_B  : NVS_KEY_LDEVID_KEY_A;

    ESP_LOGI(TAG, "Storing LDevID in slot %d", new_slot);

    // 1. Write to the inactive slot
    esp_err_t ret = nvs_mgr_set_cert(cert_nvskey, cert_pem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write cert to slot %d: %s",
                 new_slot, esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_mgr_set_cert(key_nvskey, key_pem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write key to slot %d: %s",
                 new_slot, esp_err_to_name(ret));
        return ret;
    }

    // 2. Atomic switch: if the device restarts HERE,
    //    the old slot stays active (no brick)
    ret = nvs_mgr_set_u8(NVS_KEY_LDEVID_SLOT, new_slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch slot: %s", esp_err_to_name(ret));
        return ret;
    }
    nvs_commit(h_certs);

    ESP_LOGI(TAG, "LDevID stored, active slot: %d", new_slot);
    return ESP_OK;
}

esp_err_t nvs_mgr_get_active_ldevid(char *cert_buf, size_t cert_len,
                                     char *key_buf,  size_t key_len)
{
    uint8_t slot = 0;
    nvs_mgr_get_u8(NVS_KEY_LDEVID_SLOT, &slot);

    const char *cert_nvskey = slot
        ? NVS_KEY_LDEVID_CERT_B : NVS_KEY_LDEVID_CERT_A;
    const char *key_nvskey  = slot
        ? NVS_KEY_LDEVID_KEY_B  : NVS_KEY_LDEVID_KEY_A;

    esp_err_t ret = nvs_mgr_get_cert(cert_nvskey, cert_buf, cert_len);
    if (ret != ESP_OK) return ret;
    return nvs_mgr_get_cert(key_nvskey, key_buf, key_len);
}
