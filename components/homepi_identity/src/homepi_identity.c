// components/homepi_identity/src/homepi_identity.c
// Device identity: X5C enrollment, registration with the LDevID, renewal with step-ca.
// See include/homepi_identity.h for the lifecycle.
#include "homepi_identity.h"
#include "identity_config.h"
#include "nvs_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"

static const char *TAG = "identity";

#define PEM_MAX     3072    // an LDevID chain (leaf + intermediate) is about 1.5 kB
#define CA_MAX      4096

__attribute__((weak)) void hid_on_event(hid_event_t event) { (void)event; }

// ── Helpers ───────────────────────────────────────────────────

static void base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    size_t n = 0;
    mbedtls_base64_encode((unsigned char *)out, out_len, &n, in, in_len);
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
        else if (out[i] == '=') { out[i] = '\0'; break; }
    }
}

// Certificates stored by older firmware may hold literal "\n" sequences: turn them into newlines
static void pem_unescape(char *pem)
{
    for (char *p = pem; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') {
            *p = '\n';
            memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
    }
}

static bool clock_is_set(void)
{
    return time(NULL) > 1735689600;   // 2025-01-01: NTP has run
}

static time_t crt_not_after(const mbedtls_x509_crt *crt)
{
    struct tm t = {
        .tm_year = crt->valid_to.year - 1900, .tm_mon = crt->valid_to.mon - 1,
        .tm_mday = crt->valid_to.day, .tm_hour = crt->valid_to.hour,
        .tm_min = crt->valid_to.min, .tm_sec = crt->valid_to.sec,
    };
    // valid_to is UTC and mktime() uses the local zone: at most a few hours off, enough for days
    return mktime(&t);
}

static esp_err_t load_root_ca(char *buf, size_t len)
{
    esp_err_t ret = nvs_mgr_get_cert(NVS_KEY_ROOT_CA, buf, len);
    if (ret == ESP_OK) pem_unescape(buf);
    return ret;
}

static esp_err_t load_ldevid(char *cert, size_t cert_len, char *key, size_t key_len)
{
    esp_err_t ret = nvs_mgr_get_active_ldevid(cert, cert_len, key, key_len);
    if (ret != ESP_OK || !cert[0] || !key[0]) return ESP_ERR_NOT_FOUND;
    pem_unescape(cert);
    pem_unescape(key);
    return ESP_OK;
}

// ── HTTP ──────────────────────────────────────────────────────

typedef struct { char *buf; size_t len, cap; } http_resp_t;

static esp_err_t http_on_event(esp_http_client_event_t *evt)
{
    http_resp_t *r = evt->user_data;
    if (r && evt->event_id == HTTP_EVENT_ON_DATA && r->len + evt->data_len < r->cap) {
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

// POST with an optional bearer token and an optional client certificate (mTLS through nginx)
static esp_err_t http_post(const char *url, const char *root_ca, const char *bearer,
                           const char *client_cert, const char *client_key,
                           const char *body, char *resp, size_t resp_cap, int *status)
{
    http_resp_t r = { .buf = resp, .len = 0, .cap = resp_cap };
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = root_ca,
        .client_cert_pem = client_cert,
        .client_key_pem = client_key,
        .method = HTTP_METHOD_POST,
        .event_handler = http_on_event,
        .user_data = &r,
        .timeout_ms = HID_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    static char auth[6400];
    if (bearer) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t ret = esp_http_client_perform(c);
    *status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return ret;
}

static esp_err_t http_get(const char *url, const char *root_ca, char *resp, size_t resp_cap)
{
    http_resp_t r = { .buf = resp, .len = 0, .cap = resp_cap };
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url, .cert_pem = root_ca, .method = HTTP_METHOD_GET,
        .event_handler = http_on_event, .user_data = &r, .timeout_ms = HID_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_err_t ret = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return (ret == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

// ── JWT (ES256) ───────────────────────────────────────────────

// header_name: "x5c" (enrollment, IDevID) or "x5cInsecure" (renewal, current LDevID chain).
// Every certificate of cert_pem goes into the header, leaf first: step-ca verifies the chain
// against its roots, so a renewal token needs the intermediate.
static esp_err_t sign_jwt(const char *header_name, const char *cert_pem, const char *key_pem,
                          const char *payload_json, char *jwt, size_t jwt_len)
{
    esp_err_t ret = ESP_FAIL;
    mbedtls_pk_context key;
    mbedtls_x509_crt crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    static char chain_json[4400], header_json[4500], header_b64[6100], payload_b64[1024];
    static char signing_input[7200];
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const uint8_t *)"homepi-jwt", 10) != 0 ||
        mbedtls_pk_parse_key(&key, (const unsigned char *)key_pem, strlen(key_pem) + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0 ||
        mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem, strlen(cert_pem) + 1) != 0) {
        ESP_LOGE(TAG, "JWT: certificate or key does not parse");
        goto out;
    }
    size_t used = 0;
    for (const mbedtls_x509_crt *c = &crt; c && c->raw.len; c = c->next) {
        size_t n = 0;
        if (used + 4 >= sizeof(chain_json)) goto out;
        if (used) chain_json[used++] = ',';
        chain_json[used++] = '"';
        if (mbedtls_base64_encode((unsigned char *)chain_json + used, sizeof(chain_json) - used - 2, &n,
                                  c->raw.p, c->raw.len) != 0) goto out;
        used += n;
        chain_json[used++] = '"';
    }
    chain_json[used] = '\0';
    snprintf(header_json, sizeof(header_json), "{\"alg\":\"ES256\",\"typ\":\"JWT\",\"%s\":[%s]}",
             header_name, chain_json);
    base64url_encode((const uint8_t *)header_json, strlen(header_json), header_b64, sizeof(header_b64));
    base64url_encode((const uint8_t *)payload_json, strlen(payload_json), payload_b64, sizeof(payload_b64));
    snprintf(signing_input, sizeof(signing_input), "%s.%s", header_b64, payload_b64);

    uint8_t hash[32], der[80], rs[64] = {0};
    size_t der_len = 0;
    mbedtls_sha256((const unsigned char *)signing_input, strlen(signing_input), hash, 0);
    if (mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), der, sizeof(der), &der_len,
                        mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "JWT: signing failed");
        goto out;
    }
    // DER (SEQUENCE { INTEGER r, INTEGER s }) -> fixed 64-byte r||s, left-padded
    const uint8_t *p = der;
    if (p[0] != 0x30) goto out;
    p += 2;
    for (int i = 0; i < 2; i++) {
        if (p[0] != 0x02) goto out;
        size_t l = p[1];
        p += 2;
        while (l > 32 && *p == 0x00) { p++; l--; }
        if (l > 32) goto out;
        memcpy(rs + i * 32 + (32 - l), p, l);
        p += l;
    }
    char sig_b64[100];
    base64url_encode(rs, sizeof(rs), sig_b64, sizeof(sig_b64));
    if ((size_t)snprintf(jwt, jwt_len, "%s.%s", signing_input, sig_b64) >= jwt_len) goto out;
    ret = ESP_OK;
out:
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

// ── LDevID state ──────────────────────────────────────────────

static esp_err_t ldevid_check(int *days_left, bool *expiry_known)
{
    static char cert[PEM_MAX], key[PEM_MAX];
    if (load_ldevid(cert, sizeof(cert), key, sizeof(key)) != ESP_OK) return ESP_ERR_NOT_FOUND;

    esp_err_t ret = ESP_FAIL;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const uint8_t *)"homepi-chk", 10);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert, strlen(cert) + 1) != 0 ||
        mbedtls_pk_parse_key(&pk, (const unsigned char *)key, strlen(key) + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "Stored LDevID or key does not parse");
        goto out;
    }
    if (mbedtls_pk_check_pair(&crt.pk, &pk, mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "Stored LDevID does not match its key");
        goto out;
    }
    *expiry_known = clock_is_set();
    *days_left = *expiry_known ? (int)((crt_not_after(&crt) - time(NULL)) / 86400) : 0;
    ret = ESP_OK;
out:
    memset(key, 0, sizeof(key));
    mbedtls_x509_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

bool hid_ldevid_usable(void)
{
    int days = 0;
    bool known = false;
    if (ldevid_check(&days, &known) != ESP_OK) return false;
    if (known && days < 0) {
        ESP_LOGW(TAG, "LDevID expired %d days ago", -days);
        return false;
    }
    return true;
}

esp_err_t hid_ldevid_days_left(int *days)
{
    bool known = false;
    esp_err_t ret = ldevid_check(days, &known);
    if (ret != ESP_OK) return ret;
    return known ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// step-ca answers {"crt": leaf, "ca": intermediate, "certChain": [leaf, intermediate]}. The chain is
// stored: renewal tokens must carry the intermediate, and mTLS peers get the full path.
static esp_err_t store_issued(const char *resp, const char *key_pem)
{
    static char chain[PEM_MAX];
    cJSON *r = cJSON_Parse(resp);
    if (!r) return ESP_FAIL;
    chain[0] = '\0';
    size_t total = 0;
    cJSON *list = cJSON_GetObjectItem(r, "certChain");
    if (cJSON_IsArray(list) && cJSON_GetArraySize(list) > 0) {
        cJSON *it;
        cJSON_ArrayForEach(it, list) {
            const char *pem = cJSON_GetStringValue(it);
            if (pem) total = strlcat(chain, pem, sizeof(chain));
        }
    } else {
        const char *crt = cJSON_GetStringValue(cJSON_GetObjectItem(r, "crt"));
        const char *ca = cJSON_GetStringValue(cJSON_GetObjectItem(r, "ca"));
        if (crt) total = strlcat(chain, crt, sizeof(chain));
        if (ca) total = strlcat(chain, ca, sizeof(chain));
    }
    cJSON_Delete(r);
    if (!chain[0] || total >= sizeof(chain)) {
        ESP_LOGE(TAG, "step-ca answer without a usable certificate chain");
        return ESP_FAIL;
    }
    return nvs_mgr_store_ldevid(chain, key_pem);
}

// ── Enrollment (IDevID, X5C provisioner) ──────────────────────

static esp_err_t new_key_and_csr(const char *device_id, char *csr, size_t csr_len, char *key_pem, size_t key_len)
{
    esp_err_t ret = ESP_FAIL;
    mbedtls_pk_context key;
    mbedtls_x509write_csr req;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    char subject[96];
    mbedtls_pk_init(&key);
    mbedtls_x509write_csr_init(&req);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const uint8_t *)"homepi-csr", 10) != 0 ||
        mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "EC key generation failed");
        goto out;
    }
    snprintf(subject, sizeof(subject), "CN=%s,O=HomePi,C=JP", device_id);
    mbedtls_x509write_csr_set_subject_name(&req, subject);
    mbedtls_x509write_csr_set_key(&req, &key);
    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_csr_pem(&req, (unsigned char *)csr, csr_len, mbedtls_ctr_drbg_random, &drbg) != 0 ||
        mbedtls_pk_write_key_pem(&key, (unsigned char *)key_pem, key_len) != 0) {
        ESP_LOGE(TAG, "CSR or key export failed");
        goto out;
    }
    ret = ESP_OK;
out:
    mbedtls_x509write_csr_free(&req);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

esp_err_t hid_enroll(void)
{
    static char device_id[64], root_ca[CA_MAX], idevid_cert[PEM_MAX], idevid_key[PEM_MAX];
    static char csr[PEM_MAX], key_pem[PEM_MAX], payload[512], jwt[6400], resp[6144];
    hid_on_event(HID_EVENT_ENROLLING);
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    if (!device_id[0] || load_root_ca(root_ca, sizeof(root_ca)) != ESP_OK ||
        nvs_mgr_get_cert(NVS_KEY_IDEVID_CERT, idevid_cert, sizeof(idevid_cert)) != ESP_OK ||
        nvs_mgr_get_cert(NVS_KEY_IDEVID_KEY, idevid_key, sizeof(idevid_key)) != ESP_OK) {
        ESP_LOGE(TAG, "Device id, root CA or IDevID missing from NVS");
        hid_on_event(HID_EVENT_FAILED);
        return ESP_ERR_NOT_FOUND;
    }
    pem_unescape(idevid_cert);
    pem_unescape(idevid_key);
    ESP_LOGI(TAG, "Enrolling %s with its IDevID", device_id);

    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
             "{\"sub\":\"%s\",\"iss\":\"idevid-x5c\",\"aud\":\"" HID_SIGN_AUDIENCE "\","
             "\"iat\":%lld,\"nbf\":%lld,\"exp\":%lld,\"sans\":[\"%s\"]}",
             device_id, (long long)now, (long long)now, (long long)now + 300, device_id);
    esp_err_t ret = ESP_FAIL;
    if (new_key_and_csr(device_id, csr, sizeof(csr), key_pem, sizeof(key_pem)) != ESP_OK ||
        sign_jwt("x5c", idevid_cert, idevid_key, payload, jwt, sizeof(jwt)) != ESP_OK) {
        goto done;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "csr", csr);
    cJSON_AddStringToObject(b, "ott", jwt);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    int status = 0;
    ret = http_post(HID_STEP_SIGN_URL, root_ca, NULL, NULL, NULL, body, resp, sizeof(resp), &status);
    free(body);
    if (ret != ESP_OK || (status != 200 && status != 201)) {
        ESP_LOGE(TAG, "step-ca refused the enrollment: HTTP %d", status);
        ret = ESP_FAIL;
        goto done;
    }
    ret = store_issued(resp, key_pem);
    if (ret == ESP_OK) ESP_LOGI(TAG, "LDevID issued and stored");
done:
    memset(key_pem, 0, sizeof(key_pem));
    memset(idevid_key, 0, sizeof(idevid_key));
    if (ret != ESP_OK) hid_on_event(HID_EVENT_FAILED);
    return ret;
}

// ── Registration (LDevID as client certificate) ──────────────

static void add_csv_array(cJSON *obj, const char *name, const char *csv)
{
    cJSON *a = cJSON_AddArrayToObject(obj, name);
    char tmp[256];
    strlcpy(tmp, csv && csv[0] ? csv : "unknown", sizeof(tmp));
    for (char *save = NULL, *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        cJSON_AddItemToArray(a, cJSON_CreateString(tok));
}

esp_err_t hid_register(const hid_profile_t *profile)
{
    static char device_id[64], chip_id[32], hw[128], caps[256], root_ca[CA_MAX];
    static char cert[PEM_MAX], key[PEM_MAX], resp[512];
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    nvs_mgr_get_string("chip_id", chip_id, sizeof(chip_id));
    if (profile && profile->hw_model) strlcpy(hw, profile->hw_model, sizeof(hw));
    else nvs_mgr_get_string("hw_model", hw, sizeof(hw));
    if (profile && profile->capabilities_csv) strlcpy(caps, profile->capabilities_csv, sizeof(caps));
    else nvs_mgr_get_string("capabilities", caps, sizeof(caps));
    if (load_root_ca(root_ca, sizeof(root_ca)) != ESP_OK || load_ldevid(cert, sizeof(cert), key, sizeof(key)) != ESP_OK)
        return ESP_ERR_NOT_FOUND;

    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "device_id", device_id);
    cJSON_AddStringToObject(b, "chip_id", chip_id[0] ? chip_id : "unknown");
    cJSON_AddStringToObject(b, "fw_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(b, "hw_model", hw[0] ? hw : "unknown");
#if CONFIG_IDF_TARGET_ESP32S3
    cJSON_AddStringToObject(b, "chip_target", "esp32s3");
#elif CONFIG_IDF_TARGET_ESP32C3
    cJSON_AddStringToObject(b, "chip_target", "esp32c3");
#else
    cJSON_AddStringToObject(b, "chip_target", "esp32");
#endif
    add_csv_array(b, "capabilities", caps);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    int status = 0;
    esp_err_t ret = http_post(HID_REGISTER_URL, root_ca, NULL, cert, key, body, resp, sizeof(resp), &status);
    free(body);
    memset(key, 0, sizeof(key));
    if (ret == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Registered: active");
        return ESP_OK;
    }
    if (status == 403) {
        // pending (not pre-announced) or suspended: the operator decides, nothing to retry here
        ESP_LOGW(TAG, "Registration refused: %.120s", resp);
        return ESP_ERR_NOT_ALLOWED;
    }
    ESP_LOGW(TAG, "Registration failed: %s, HTTP %d", esp_err_to_name(ret), status);
    return ESP_FAIL;
}

// ── Renewal (step-ca, token signed by the current LDevID key) ─

esp_err_t hid_renew_if_due(bool force, bool *renewed)
{
    static char device_id[64], root_ca[CA_MAX], cert[PEM_MAX], key[PEM_MAX], inter[PEM_MAX];
    static char payload[512], jwt[6400], resp[6144];
    *renewed = false;
    int days = 0;
    esp_err_t ret = hid_ldevid_days_left(&days);
    if (ret != ESP_OK) return ret;   // no LDevID, or clock not set yet
    if (!force && days > HID_RENEW_DAYS_BEFORE) {
        ESP_LOGI(TAG, "LDevID valid for %d more days, no renewal", days);
        return ESP_OK;
    }
    hid_on_event(HID_EVENT_RENEWING);
    ESP_LOGI(TAG, "Renewing the LDevID (%d days left%s)", days, force ? ", forced" : "");
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    if (load_root_ca(root_ca, sizeof(root_ca)) != ESP_OK || load_ldevid(cert, sizeof(cert), key, sizeof(key)) != ESP_OK)
        return ESP_ERR_NOT_FOUND;

    // An LDevID stored by v2.3.x holds the leaf only: step-ca needs the intermediate in the token
    const char *end = strstr(cert, "-----END CERTIFICATE-----");
    if (end && !strstr(end, "-----BEGIN CERTIFICATE-----")) {
        if (http_get(HID_STEP_INTERMEDIATES_URL, root_ca, inter, sizeof(inter)) != ESP_OK ||
            !strstr(inter, "-----BEGIN CERTIFICATE-----") || strlen(cert) + strlen(inter) + 2 >= sizeof(cert)) {
            ESP_LOGE(TAG, "Intermediate certificate unavailable: cannot build the renewal token");
            ret = ESP_FAIL;
            goto done;
        }
        if (cert[strlen(cert) - 1] != '\n') strlcat(cert, "\n", sizeof(cert));
        strlcat(cert, inter, sizeof(cert));
        ESP_LOGI(TAG, "Leaf-only LDevID (v2.3.x): intermediate appended for the renewal token");
    }

    uint8_t rnd[16];
    char jti[33];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) snprintf(jti + 2 * i, 3, "%02x", rnd[i]);
    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
             "{\"sub\":\"%s\",\"iss\":\"" HID_RENEW_ISSUER "\",\"aud\":\"" HID_RENEW_AUDIENCE "\","
             "\"iat\":%lld,\"nbf\":%lld,\"exp\":%lld,\"jti\":\"%s\"}",
             device_id, (long long)now, (long long)now, (long long)now + 300, jti);
    ret = sign_jwt("x5cInsecure", cert, key, payload, jwt, sizeof(jwt));
    if (ret != ESP_OK) goto done;
    int status = 0;
    ret = http_post(HID_STEP_RENEW_URL, root_ca, jwt, NULL, NULL, "{}", resp, sizeof(resp), &status);
    if (ret != ESP_OK || (status != 200 && status != 201)) {
        ESP_LOGE(TAG, "step-ca refused the renewal: HTTP %d", status);
        ret = ESP_FAIL;
        goto done;
    }
    // Same key, new certificate: stored in the other A/B slot, then the slot switches
    ret = store_issued(resp, key);
    if (ret == ESP_OK) {
        *renewed = true;
        memset(key, 0, sizeof(key));
        if (hid_ldevid_days_left(&days) == ESP_OK) ESP_LOGI(TAG, "LDevID renewed: valid for %d days", days);
        return ESP_OK;
    }
done:
    memset(key, 0, sizeof(key));
    hid_on_event(HID_EVENT_FAILED);
    return ret;
}
