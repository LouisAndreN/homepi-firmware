// main/enrollment.c
#include "enrollment.h"
#include "config.h"
#include "nvs_manager.h"
#include "led.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "mbedtls/pem.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"

static const char *TAG = "enroll";

// ── Base64url (JWT sans padding) ──────────────────────────────

static void base64url_encode(const uint8_t *in, size_t in_len,
                              char *out, size_t out_len)
{
    size_t b64_len = 0;
    mbedtls_base64_encode((unsigned char*)out, out_len, &b64_len, in, in_len);
    out[b64_len] = '\0';
    // Remplacer +→-, /→_, supprimer =
    for (size_t i = 0; i < b64_len; i++) {
        if      (out[i] == '+')  out[i] = '-';
        else if (out[i] == '/')  out[i] = '_';
        else if (out[i] == '=')  out[i] = '\0';
    }
}

// ── Génération CSR ────────────────────────────────────────────

static esp_err_t generate_ec_keypair_and_csr(
    const char *device_id,
    char *csr_pem, size_t csr_len,
    char *key_pem, size_t key_len)
{
    mbedtls_pk_context          key;
    mbedtls_entropy_context     entropy;
    mbedtls_ctr_drbg_context    ctr_drbg;
    mbedtls_x509write_csr       csr;
    esp_err_t                   ret = ESP_FAIL;
    char                        subject[64];

    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509write_csr_init(&csr);

    // Seed PRNG
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
            (const uint8_t*)"homepi-csr", 10) != 0) {
        ESP_LOGE(TAG, "PRNG seed failed");
        goto cleanup;
    }

    // Générer clé EC P-256
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "EC key generation failed");
        goto cleanup;
    }

    // Configurer le CSR
    snprintf(subject, sizeof(subject), "CN=%s,O=HomePi,C=JP", device_id);
    mbedtls_x509write_csr_set_subject_name(&csr, subject);
    mbedtls_x509write_csr_set_key(&csr, &key);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);

    // Exporter CSR PEM
    if (mbedtls_x509write_csr_pem(&csr, (unsigned char*)csr_pem, csr_len,
            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "CSR PEM export failed");
        goto cleanup;
    }

    // Exporter clé privée PEM
    if (mbedtls_pk_write_key_pem(&key, (unsigned char*)key_pem, key_len) != 0) {
        ESP_LOGE(TAG, "Key PEM export failed");
        goto cleanup;
    }

    ret = ESP_OK;
    ESP_LOGI(TAG, "CSR generated for CN=%s", device_id);

cleanup:
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&key);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return ret;
}

// ── HMAC-SHA256 ───────────────────────────────────────────────

static void compute_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *msg, size_t msg_len,
                                  uint8_t *digest)
{
    mbedtls_md_hmac(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        key, key_len, msg, msg_len, digest
    );
}

static void bytes_to_hex(const uint8_t *bytes, size_t len,
                           char *hex, size_t hex_len)
{
    for (size_t i = 0; i < len && i*2+2 < hex_len; i++)
        snprintf(hex + i*2, 3, "%02x", bytes[i]);
}

// ── JWT X5C pour step-ca ──────────────────────────────────────
// Format : header.payload.signature (base64url)
// Header : {"alg":"ES256","x5c":["<base64(DER(idevid_cert))>"]}
// Payload: {"sub":"<device_id>","iss":"idevid-x5c","aud":"<ca_url>/1.0/sign",
//           "iat":<now>,"nbf":<now>,"exp":<now+300>}

static esp_err_t create_x5c_jwt(
    const char *device_id,
    const char *idevid_cert_pem,
    const char *idevid_key_pem,
    const char *csr_pem,
    char *jwt_out, size_t jwt_len)
{
    esp_err_t   ret = ESP_FAIL;
    mbedtls_pk_context         idevid_key;
    mbedtls_entropy_context    entropy;
    mbedtls_ctr_drbg_context   ctr_drbg;
    mbedtls_x509_crt           idevid_crt;

    mbedtls_pk_init(&idevid_key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&idevid_crt);

    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                           (const uint8_t*)"homepi-jwt", 10);

    // Parser la clé IDevID
    if (mbedtls_pk_parse_key(&idevid_key,
            (const unsigned char*)idevid_key_pem,
            strlen(idevid_key_pem)+1, NULL, 0,
            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "Failed to parse IDevID key");
        goto cleanup;
    }

    // Parser le cert IDevID pour obtenir le DER
    if (mbedtls_x509_crt_parse(&idevid_crt,
            (const unsigned char*)idevid_cert_pem,
            strlen(idevid_cert_pem)+1) != 0) {
        ESP_LOGE(TAG, "Failed to parse IDevID cert");
        goto cleanup;
    }

    // Encoder le DER du cert en base64 (standard, pour x5c)
    static char cert_b64[4096];
    size_t cert_b64_len = 0;
    mbedtls_base64_encode(
        (unsigned char*)cert_b64, sizeof(cert_b64), &cert_b64_len,
        idevid_crt.raw.p, idevid_crt.raw.len);
    cert_b64[cert_b64_len] = '\0';
    ESP_LOGI(TAG, "cert_b64 len=%d", (int)cert_b64_len);

    // ── Header JSON → base64url ──────────────────────────────
    static char header_json[5000];
    snprintf(header_json, sizeof(header_json),
             "{\"alg\":\"ES256\",\"x5c\":[\"%s\"]}", cert_b64);
    ESP_LOGI(TAG, "header_json len=%d", (int)strlen(header_json));
    static char header_b64[8192];
    base64url_encode((uint8_t*)header_json, strlen(header_json),
                     header_b64, sizeof(header_b64));
    ESP_LOGI(TAG, "header_b64 len=%d", (int)strlen(header_b64));

    // ── Payload JSON → base64url ─────────────────────────────
    time_t now = time(NULL);
    static char payload_json[2048];
    snprintf(payload_json, sizeof(payload_json),
             "{\"sub\":\"%.63s\","
             "\"iss\":\"idevid-x5c\","
             "\"aud\":\"" STEP_CA_AUDIENCE "\","
             "\"iat\":%lld,\"nbf\":%lld,\"exp\":%lld,"
             "\"sans\":[\"%.63s\"]}",
             device_id, (long long)now, (long long)now, (long long)(now+300),
             device_id);
    static char payload_b64[1024];
    base64url_encode((uint8_t*)payload_json, strlen(payload_json),
                     payload_b64, sizeof(payload_b64));
    ESP_LOGI(TAG, "payload_b64 len=%d", (int)strlen(payload_b64));

    // ── Signing input : "header.payload" ─────────────────────
    static char signing_input[12288];   // header_json fait ~5000 + overhead base64url
    snprintf(signing_input, sizeof(signing_input),
             "%s.%s", header_b64, payload_b64);
    ESP_LOGI(TAG, "signing_input len=%d", (int)strlen(signing_input));

    // SHA256 du signing input
    uint8_t hash[32];
    mbedtls_sha256((unsigned char*)signing_input,
                   strlen(signing_input), hash, 0);

    // Signer avec ECDSA (ES256)
    uint8_t sig_der[72];
    size_t  sig_der_len = 0;
    if (mbedtls_pk_sign(&idevid_key, MBEDTLS_MD_SHA256,
            hash, 32, sig_der, sizeof(sig_der), &sig_der_len,
            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "JWT signing failed");
        goto cleanup;
    }

    // Convertir DER → R||S (format JWT ES256 = 64 bytes fixes)
    uint8_t sig_rs[64] = {0};
    // Parser la structure DER : 0x30 len 0x02 rlen R 0x02 slen S
    const uint8_t *p = sig_der;
    if (p[0] != 0x30) { ESP_LOGE(TAG, "Bad DER sig"); goto cleanup; }
    p += 2; // skip 0x30 + total_len
    if (p[0] != 0x02) { ESP_LOGE(TAG, "Bad DER R tag"); goto cleanup; }
    size_t r_len = p[1]; p += 2;
    // R peut avoir un 0x00 prefix si le MSB est set
    if (r_len == 33 && p[0] == 0x00) { r_len = 32; p++; }
    memcpy(sig_rs, p, r_len); p += r_len;
    if (p[0] != 0x02) { ESP_LOGE(TAG, "Bad DER S tag"); goto cleanup; }
    size_t s_len = p[1]; p += 2;
    if (s_len == 33 && p[0] == 0x00) { s_len = 32; p++; }
    memcpy(sig_rs + 32, p, s_len);

    // Encoder R||S en base64url
    static char sig_b64[200];
    base64url_encode(sig_rs, 64, sig_b64, sizeof(sig_b64));

    // ── Assembler le JWT ──────────────────────────────────────
    snprintf(jwt_out, jwt_len, "%s.%s.%s",
             header_b64, payload_b64, sig_b64);

    ret = ESP_OK;
    ESP_LOGI(TAG, "X5C JWT created");
    ESP_LOGI(TAG, "JWT final len=%d", (int)strlen(jwt_out));

cleanup:
    mbedtls_pk_free(&idevid_key);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_x509_crt_free(&idevid_crt);
    return ret;
}

// ── HTTP POST JSON ────────────────────────────────────────────

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t*)evt->user_data;
    if (!resp) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = resp->len + evt->data_len + 1;
        if (needed > resp->cap) {
            resp->buf = realloc(resp->buf, needed + 512);
            resp->cap = needed + 512;
        }
        if (resp->buf) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, const char *root_ca,
                                char *resp_buf, size_t resp_cap,
                                int *status_code)
{
    http_resp_t resp = { .buf = calloc(1, 512), .len = 0, .cap = 512 };

    esp_http_client_config_t cfg = {
        .url           = url,
        .cert_pem      = root_ca,
        .method        = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data     = &resp,
        .timeout_ms    = X5C_TIMEOUT_MS,
        .transport_type              = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size                 = 4096,                     
        .buffer_size_tx              = 4096,         
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t ret = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && resp.buf && resp.len > 0) {
        size_t copy_len = resp.len < resp_cap - 1 ? resp.len : resp_cap - 1;
        memcpy(resp_buf, resp.buf, copy_len);
        resp_buf[copy_len] = '\0';
    }
    free(resp.buf);
    return ret;
}


static esp_err_t http_post_json(const char *url, const char *root_ca,
                                  const char *body,
                                  char *resp_buf, size_t resp_cap,
                                  int *status_code)
{
    http_resp_t resp = { .buf = calloc(1, 512), .len = 0, .cap = 512 };

    esp_http_client_config_t cfg = {
        .url                         = url,
        .cert_pem                    = root_ca,
        .method                      = HTTP_METHOD_POST,
        .event_handler               = http_event_handler,
        .user_data                   = &resp,
        .timeout_ms                  = X5C_TIMEOUT_MS,
        .skip_cert_common_name_check = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t ret = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && resp.buf && resp.len > 0) {
        size_t copy_len = resp.len < resp_cap - 1 ? resp.len : resp_cap - 1;
        memcpy(resp_buf, resp.buf, copy_len);
        resp_buf[copy_len] = '\0';
    }
    free(resp.buf);
    return ret;
}


// ── Notifier l'API après enrollment réussi ────────────────────
void notify_api_enrolled(const char *device_id, const char *ldevid_pem)
{
    // Copier et unescape les \n littéraux
    char pem_buf[2048];
    strncpy(pem_buf, ldevid_pem, sizeof(pem_buf)-1);
    pem_buf[sizeof(pem_buf)-1] = '\0';
    // Remplacer \n littéraux par vrais sauts de ligne
    for (char *p = pem_buf; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') {
            *p = '\n';
            memmove(p+1, p+2, strlen(p+2)+1);
        }
    }


    // Parser le LDevID pour extraire fingerprint + expiry
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt,
            (const unsigned char*)pem_buf,
            strlen(pem_buf)+1) != 0) {
        ESP_LOGW(TAG, "notify_api: failed to parse LDevID");
        return;
    }

    // Fingerprint SHA256 du DER
    uint8_t fp[32];
    mbedtls_sha256(crt.raw.p, crt.raw.len, fp, 0);
    char fp_hex[65] = {0};
    for (int i = 0; i < 32; i++)
        snprintf(fp_hex + i*2, 3, "%02x", fp[i]);

    // Expiry ISO8601
    char expiry[32];
    snprintf(expiry, sizeof(expiry), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             crt.valid_to.year, crt.valid_to.mon,  crt.valid_to.day,
             crt.valid_to.hour, crt.valid_to.min,  crt.valid_to.sec);

    mbedtls_x509_crt_free(&crt);

    char hw_model[128] = {0};
    nvs_mgr_get_string("hw_model", hw_model, sizeof(hw_model));

    char chip_id[32] = {0};
    nvs_mgr_get_string("chip_id", chip_id, sizeof(chip_id));

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id",        device_id);
    cJSON_AddStringToObject(body, "chip_id",          chip_id[0] ? chip_id : "unknown");
    cJSON_AddStringToObject(body, "fw_version",       esp_app_get_description()->version);
    cJSON_AddStringToObject(body, "hw_model",         hw_model[0] ? hw_model : "unknown"); // ← SEUL AJOUT
    cJSON_AddStringToObject(body, "cert_fingerprint", fp_hex);
    cJSON_AddStringToObject(body, "cert_expiry",      expiry);
    
    #ifdef CONFIG_IDF_TARGET_ESP32S3
        cJSON_AddStringToObject(body, "chip_target", "esp32s3");
    #elif CONFIG_IDF_TARGET_ESP32C3
        cJSON_AddStringToObject(body, "chip_target", "esp32c3");
    #else
        cJSON_AddStringToObject(body, "chip_target", "esp32");
    #endif

    // capabilities depuis NVS — inchangé
    char caps_csv[256] = {0};
    nvs_mgr_get_string("capabilities", caps_csv, sizeof(caps_csv));
    cJSON *caps = cJSON_AddArrayToObject(body, "capabilities");
    if (caps_csv[0]) {
        char tmp[256];
        strncpy(tmp, caps_csv, sizeof(tmp)-1);
        char *tok = strtok(tmp, ",");
        while (tok) {
            cJSON_AddItemToArray(caps, cJSON_CreateString(tok));
            tok = strtok(NULL, ",");
        }
    } else {
        cJSON_AddItemToArray(caps, cJSON_CreateString("unknown"));
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    static char root_ca[4096];
    nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca));

    static char resp[256];
    int status = 0;
    http_post_json("https://" HOMEPI_API_HOST "/api/devices/x5c-enrolled",
                   root_ca, 
                   body_str, resp, sizeof(resp), &status);
    free(body_str);

    if (status == 200 || status == 201)
        ESP_LOGI(TAG, "Device registered in API — status: active");
    else
        ESP_LOGW(TAG, "API notification failed: HTTP %d (non-fatal)", status);
}



// ── Zero-touch X5C ───────────────────────────────────────────

esp_err_t enrollment_zero_touch(void)
{
    led_set(LED_ZERO_TOUCH);
    ESP_LOGI(TAG, "=== Zero-touch X5C enrollment ===");

    // Charger depuis NVS
    static char device_id[64], root_ca[4096];
    static char idevid_cert[2048], idevid_key[2048];
    nvs_mgr_get_string(NVS_KEY_DEVICE_ID,  device_id,   sizeof(device_id));
    nvs_mgr_get_cert  (NVS_KEY_ROOT_CA,    root_ca,     sizeof(root_ca));
    nvs_mgr_get_cert  (NVS_KEY_IDEVID_CERT,idevid_cert, sizeof(idevid_cert));
    nvs_mgr_get_cert  (NVS_KEY_IDEVID_KEY, idevid_key,  sizeof(idevid_key));

    ESP_LOGI(TAG, "idevid_cert len=%d preview=%.40s",
         (int)strlen(idevid_cert), idevid_cert);
    ESP_LOGI(TAG, "idevid_key len=%d preview=%.40s",
            (int)strlen(idevid_key), idevid_key);

    if (strlen(device_id) == 0 || strlen(idevid_cert) == 0) {
        ESP_LOGE(TAG, "IDevID or device_id missing from NVS");
        return ESP_ERR_NOT_FOUND;
    }

    // Générer nouvelle clé + CSR
    static char csr_pem[2048], new_key_pem[2048];
    esp_err_t ret = generate_ec_keypair_and_csr(device_id,
        csr_pem, sizeof(csr_pem), new_key_pem, sizeof(new_key_pem));
    if (ret != ESP_OK) return ret;

    // Créer JWT X5C
    static char jwt[4096];
    ret = create_x5c_jwt(device_id, idevid_cert, idevid_key,
                         csr_pem, jwt, sizeof(jwt));
    if (ret != ESP_OK) return ret;

    // Construire le body JSON pour /step/sign
    cJSON *body_json = cJSON_CreateObject();
    cJSON_AddStringToObject(body_json, "csr", csr_pem);
    cJSON_AddStringToObject(body_json, "ott", jwt);
    char *body = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);

    // ESP_LOGI(TAG, "POST body (first 200): %.200s", body);
    // ESP_LOGI(TAG, "JWT len=%d first 80: %.80s", (int)strlen(jwt), jwt);

    // POST /step/sign
    static char resp_buf[4096];
    int status_code = 0;
    ret = http_post_json(STEP_SIGN_URL, root_ca, body,
                         resp_buf, sizeof(resp_buf), &status_code);
    free(body);

    if (ret != ESP_OK || (status_code != 200 && status_code != 201)) {
        ESP_LOGE(TAG, "X5C POST failed: HTTP %d", status_code);
        return ESP_FAIL;
    }

    // Parser la réponse : {"crt":"<pem>"}
    cJSON *resp = cJSON_Parse(resp_buf);
    if (!resp) { ESP_LOGE(TAG, "Invalid JSON response"); return ESP_FAIL; }

    const char *ldevid_pem_raw = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "crt"));
    if (!ldevid_pem_raw) {
        ESP_LOGE(TAG, "Missing 'crt' in response");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    static char ldevid_pem[2048];
    strlcpy(ldevid_pem, ldevid_pem_raw, sizeof(ldevid_pem));
    cJSON_Delete(resp);  // ← libérer maintenant, ldevid_pem est indépendant

    // Stocker LDevID (A/B swap atomique)
    ret = nvs_mgr_store_ldevid(ldevid_pem, new_key_pem);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "heap before notify: %lu bytes free",
                 (unsigned long)esp_get_free_heap_size());
        ESP_LOGI(TAG, "ldevid_pem len=%d", (int)strlen(ldevid_pem));
        ESP_LOGI(TAG, "ldevid_pem first 60: %.60s", ldevid_pem);
        
        notify_api_enrolled(device_id, ldevid_pem);
        nvs_mgr_set_u8(NVS_KEY_ENROLLED, 1);
        ESP_LOGI(TAG, "Zero-touch SUCCESS — LDevID stored, rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    return ret;
}

// ── Fallback HMAC ─────────────────────────────────────────────

esp_err_t enrollment_fallback(void)
{
    led_set(LED_FALLBACK_WAIT);
    ESP_LOGI(TAG, "=== Fallback HMAC enrollment ===");

    static char device_id[64], root_ca[4096];
    uint8_t     hmac_key[32];
    size_t      hmac_len = sizeof(hmac_key);

    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    nvs_mgr_get_cert  (NVS_KEY_ROOT_CA,   root_ca,   sizeof(root_ca));
    nvs_mgr_get_binary(NVS_KEY_HMAC_KEY,  hmac_key,  &hmac_len);

    // Nonce = timestamp en hex
    char nonce[32];
    snprintf(nonce, sizeof(nonce), "%lld", (long long)esp_timer_get_time());

    // HMAC proof = HMAC-SHA256(hmac_key, "{device_id}:{nonce}")
    char msg[128];
    snprintf(msg, sizeof(msg), "%s:%s", device_id, nonce);
    uint8_t digest[32];
    compute_hmac_sha256(hmac_key, 32, (uint8_t*)msg, strlen(msg), digest);
    char proof_hex[65];
    bytes_to_hex(digest, 32, proof_hex, sizeof(proof_hex));

    // Construire capabilities depuis NVS ou défaut
    cJSON *announce = cJSON_CreateObject();
    cJSON_AddStringToObject(announce, "device_id",  device_id);

    char chip_id_buf[32] = {0};
    nvs_mgr_get_string("chip_id", chip_id_buf, sizeof(chip_id_buf));
    cJSON_AddStringToObject(announce, "chip_id", chip_id_buf[0] ? chip_id_buf : "unknown");

    char hw_model_buf[32] = {0};
    nvs_mgr_get_string("hw_model", hw_model_buf, sizeof(hw_model_buf));
    cJSON_AddStringToObject(announce, "hw_model", hw_model_buf[0] ? hw_model_buf : "esp32c3");

    cJSON_AddStringToObject(announce, "fw_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(announce, "nonce",      nonce);
    cJSON_AddStringToObject(announce, "hmac_proof", proof_hex);

    cJSON *caps = cJSON_AddArrayToObject(announce, "capabilities");
    char caps_csv[256] = {0};
    nvs_mgr_get_string("capabilities", caps_csv, sizeof(caps_csv));
    if (caps_csv[0]) {
        char tmp[256];
        strncpy(tmp, caps_csv, sizeof(tmp)-1);
        char *tok = strtok(tmp, ",");
        while (tok) {
            cJSON_AddItemToArray(caps, cJSON_CreateString(tok));
            tok = strtok(NULL, ",");
        }
    } else {
        cJSON_AddItemToArray(caps, cJSON_CreateString("unknown"));
    }

    char *announce_body = cJSON_PrintUnformatted(announce);
    cJSON_Delete(announce);

    // POST /api/devices/announce
    static char resp_buf[512];
    int status_code = 0;
    char announce_url[128];
    snprintf(announce_url, sizeof(announce_url),
             "https://%s/api/devices/announce", HOMEPI_API_HOST);

    esp_err_t ret = http_post_json(announce_url, root_ca, announce_body,
                                   resp_buf, sizeof(resp_buf), &status_code);
    free(announce_body);

    if (ret != ESP_OK || (status_code != 200 && status_code != 201)) {
        ESP_LOGE(TAG, "Announce failed: HTTP %d", status_code);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Announced — waiting for operator approval...");

    // Poll /api/devices/{device_id}/status toutes les 5s (2 min max)
    char status_url[128];
    snprintf(status_url, sizeof(status_url),
             "https://%s/api/devices/%s/status", HOMEPI_API_HOST, device_id);

    bool approved = false;
    for (int i = 0; i < 24; i++) {  // 24 × 5s = 2 min
        vTaskDelay(pdMS_TO_TICKS(FALLBACK_POLL_MS));
        memset(resp_buf, 0, sizeof(resp_buf));

        // GET via POST vide (workaround — utiliser GET en production)
        // TODO: utiliser esp_http_client_perform avec GET
        // Pour simplifier, réutiliser http_post_json avec body vide
        // ret = http_post_json(status_url, root_ca, "{}",
                            //  resp_buf, sizeof(resp_buf), &status_code);

        ret = http_get_json(status_url, root_ca, resp_buf, sizeof(resp_buf), &status_code);

        if (ret == ESP_OK && status_code == 200) {
            cJSON *status_json = cJSON_Parse(resp_buf);
            const char *status = cJSON_GetStringValue(
                cJSON_GetObjectItem(status_json, "status"));
            if (status && strcmp(status, "approved") == 0) {
                approved = true;
                cJSON_Delete(status_json);
                break;
            }
            cJSON_Delete(status_json);
        }
        ESP_LOGI(TAG, "Waiting for approval... (%d/24)", i+1);
    }

    if (!approved) {
        ESP_LOGW(TAG, "Approval timeout — will retry on next boot");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Approved! Generating CSR for enrollment...");

    // Générer CSR + nouvelle clé
    static char csr_pem[2048], new_key_pem[2048];
    ret = generate_ec_keypair_and_csr(device_id, csr_pem, sizeof(csr_pem),
                                       new_key_pem, sizeof(new_key_pem));
    if (ret != ESP_OK) return ret;

    // POST /api/devices/enroll
    char enroll_url[128];
    snprintf(enroll_url, sizeof(enroll_url),
             "https://%s/api/devices/enroll", HOMEPI_API_HOST);

    cJSON *enroll_body = cJSON_CreateObject();
    cJSON_AddStringToObject(enroll_body, "device_id", device_id);
    cJSON_AddStringToObject(enroll_body, "csr_pem",   csr_pem);
    char *enroll_str = cJSON_PrintUnformatted(enroll_body);
    cJSON_Delete(enroll_body);

    memset(resp_buf, 0, sizeof(resp_buf));
    ret = http_post_json(enroll_url, root_ca, enroll_str,
                         resp_buf, sizeof(resp_buf), &status_code);
    free(enroll_str);

    if (ret != ESP_OK || (status_code != 200 && status_code != 201)) {
        ESP_LOGE(TAG, "Enroll failed: HTTP %d", status_code);
        return ESP_FAIL;
    }

    // Parser la réponse : {"device_id":"...","cert_pem":"..."}
    cJSON *enroll_resp = cJSON_Parse(resp_buf);
    const char *ldevid_pem = cJSON_GetStringValue(
        cJSON_GetObjectItem(enroll_resp, "cert_pem"));

    if (!ldevid_pem) {
        ESP_LOGE(TAG, "Missing cert_pem in enroll response");
        cJSON_Delete(enroll_resp);
        return ESP_FAIL;
    }

    // Stocker LDevID + activer
    ret = nvs_mgr_store_ldevid(ldevid_pem, new_key_pem);
    cJSON_Delete(enroll_resp);

    if (ret == ESP_OK) {
        notify_api_enrolled(device_id, ldevid_pem);
        nvs_mgr_set_u8(NVS_KEY_ENROLLED, 1);
        ESP_LOGI(TAG, "Fallback enrollment SUCCESS — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    return ret;
}

// ── Vérification et renewal J-30 ─────────────────────────────

esp_err_t enrollment_check_renewal(void)
{
    static char ldevid_cert[2048], ldevid_key[2048];
    static char device_id[64], root_ca[4096];

    nvs_mgr_get_string(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca));
    nvs_mgr_get_active_ldevid(ldevid_cert, sizeof(ldevid_cert),
                               ldevid_key,  sizeof(ldevid_key));

    // Parser le cert pour vérifier la date d'expiry
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt,
            (const unsigned char*)ldevid_cert,
            strlen(ldevid_cert)+1) != 0) {
        ESP_LOGE(TAG, "Failed to parse LDevID for renewal check");
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }

    // Calculer les jours restants
    time_t now = time(NULL);
    struct tm exp_tm = {
        .tm_year = crt.valid_to.year  - 1900,
        .tm_mon  = crt.valid_to.mon   - 1,
        .tm_mday = crt.valid_to.day,
        .tm_hour = crt.valid_to.hour,
        .tm_min  = crt.valid_to.min,
        .tm_sec  = crt.valid_to.sec,
    };
    time_t exp_time = mktime(&exp_tm);
    int days_remaining = (int)((exp_time - now) / 86400);

    ESP_LOGI(TAG, "LDevID expires in %d days", days_remaining);
    mbedtls_x509_crt_free(&crt);

    if (days_remaining > CERT_RENEW_DAYS_BEFORE) {
        ESP_LOGI(TAG, "No renewal needed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Renewal needed — %d days left", days_remaining);

    // Générer nouveau CSR
    static char csr_pem[2048], new_key_pem[2048];
    esp_err_t ret = generate_ec_keypair_and_csr(device_id,
        csr_pem, sizeof(csr_pem), new_key_pem, sizeof(new_key_pem));
    if (ret != ESP_OK) return ret;

    // POST /api/devices/{id}/renew
    char renew_url[128];
    snprintf(renew_url, sizeof(renew_url),
             "https://%s/api/devices/%s/renew", HOMEPI_API_HOST, device_id);

    cJSON *body_json = cJSON_CreateObject();
    cJSON_AddStringToObject(body_json, "device_id", device_id);
    cJSON_AddStringToObject(body_json, "csr_pem",   csr_pem);
    char *body = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);

    nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca));

    static char resp_buf[4096];
    int status_code = 0;
    ret = http_post_json(renew_url, root_ca, body,
                         resp_buf, sizeof(resp_buf), &status_code);
    free(body);

    if (ret != ESP_OK || (status_code != 200 && status_code != 201)) {
        ESP_LOGE(TAG, "Renewal failed: HTTP %d", status_code);
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_Parse(resp_buf);
    const char *new_cert = cJSON_GetStringValue(
        cJSON_GetObjectItem(resp, "cert_pem"));

    if (!new_cert) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    ret = nvs_mgr_store_ldevid(new_cert, new_key_pem);
    cJSON_Delete(resp);

    if (ret == ESP_OK)
        ESP_LOGI(TAG, "LDevID renewed successfully");

    return ret;
}