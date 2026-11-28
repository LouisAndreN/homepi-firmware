// main/teensy_ota.c: remote update of the Teensy 4.0 (v2.6.0). See teensy_ota.h and fxproto.h.
#include "teensy_ota.h"
#include "mqtt_app.h"
#include "nilm_bridge.h"
#include "nvs_manager.h"
#include "homepi_ota.h"
#include "led.h"
#include "fx_sender.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "teensy_ota";

#define TOTA_PROJECT        "homepi_nilm_teensy"
#define TOTA_CHIP           "teensy40"
#define TOTA_MAX_IMAGE      (1024 * 1024)   // the Teensy 4.0 has 2 MB; the program and its copy fit
#define TOTA_BOOT_WAIT_MS   20000           // new program: first data frame expected within this
#define NVS_KEY_ROOT_CA     "root_ca"

// ── Queue: one writer (MQTT task), one reader (main task) ──
static struct {
    char url[256];
    hota_manifest_t m;
} s_req;
static bool s_pending;

static void publish_failure(const char *reason, const char *detail, const char *version, const char *running)
{
    char json[320];
    snprintf(json, sizeof(json),
             "{\"type\":\"teensy_ota_failed\",\"reason\":\"%s\",\"detail\":\"%s\",\"version\":\"%s\",\"running\":\"%s\",\"ts\":%lld}",
             reason, detail ? detail : "", version ? version : "", running ? running : "", (long long)time(NULL));
    ESP_LOGE(TAG, "Teensy update refused or failed: %s %s", reason, detail ? detail : "");
    mqtt_app_publish_status(json);
}

// vMAJOR.MINOR.PATCH -> major, minor (the Teensy reports major.minor; the patch must be 0)
static bool teensy_version(const char *v, uint8_t *major, uint8_t *minor)
{
    unsigned a, b, c;
    char tail;
    if (sscanf(v, "v%u.%u.%u%c", &a, &b, &c, &tail) != 3 || a > 255 || b > 99 || c != 0) return false;
    *major = (uint8_t)a; *minor = (uint8_t)b;
    return true;
}

void teensy_ota_handle_command(const char *url, const char *manifest, const char *signature)
{
    if (__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE)) { publish_failure("teensy_ota_in_progress", NULL, NULL, NULL); return; }
    hota_manifest_t m;
    const char *why = NULL;
    if (hota_verify_signed(manifest, signature, &m, &why) != ESP_OK) { publish_failure(why, NULL, NULL, NULL); return; }
    uint8_t maj, min;
    if (strcmp(m.project, TOTA_PROJECT) != 0) { publish_failure("wrong_project", m.project, m.version, NULL); return; }
    if (strcmp(m.chip_target, TOTA_CHIP) != 0) { publish_failure("wrong_chip", m.chip_target, m.version, NULL); return; }
    if (!teensy_version(m.version, &maj, &min)) { publish_failure("version_not_comparable", NULL, m.version, NULL); return; }
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(s_req.url)) { publish_failure("bad_url", NULL, m.version, NULL); return; }

    strlcpy(s_req.url, url, sizeof(s_req.url));
    s_req.m = m;
    __atomic_store_n(&s_pending, true, __ATOMIC_RELEASE);

    char ack[160];
    snprintf(ack, sizeof(ack), "{\"type\":\"teensy_ota_received\",\"version\":\"%s\",\"ts\":%lld}", m.version, (long long)time(NULL));
    mqtt_app_publish_status(ack);
    ESP_LOGI(TAG, "Teensy %s queued for the main task", m.version);
}

// ── fx_sender transport: UART + reply queue of nilm_bridge ──
static int io_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; return nilm_bridge_write(b, n) == ESP_OK ? 0 : -1; }
static int io_wait(void *ctx, uint16_t seq, uint8_t cmd, uint32_t ms, uint8_t *st, uint32_t *val)
{
    (void)ctx;
    return nilm_bridge_wait_reply(seq, cmd, ms, st, val) ? 0 : -1;
}

// Runs the whole update; returns NULL on success or the reason (detail filled)
static const char *run(const hota_manifest_t *m, const char *url, char *detail, size_t dlen, char *previous, size_t plen,
                       bool *started)
{
    *started = false;
    static fxs_t s;                    // ~2 KB: not on the stack
    static uint8_t rbuf[1024];
    static char root_ca[4096];
    memset(&s, 0, sizeof(s));
    s.io = (fxs_io_t){ io_send, io_wait, NULL };
    nilm_bridge_flush_replies();

    // 1. What the Teensy runs now (PING works whatever its sampling state)
    uint32_t fw = 0;
    int r = fxs_ping(&s, &fw);
    if (r != 0) { snprintf(detail, dlen, "ping: %s", fxs_status_name(r)); return "teensy_no_reply"; }
    uint8_t cur_maj = (uint8_t)(fw >> 8), cur_min = (uint8_t)fw, new_maj, new_min;
    snprintf(previous, plen, "v%u.%u.0", cur_maj, cur_min);
    teensy_version(m->version, &new_maj, &new_min);
    bool ok_cmp = false;
    if (hota_version_cmp(m->version, previous, &ok_cmp) <= 0 || !ok_cmp) { snprintf(detail, dlen, "running %s", previous); return "not_newer"; }

    // 2. Download, hash and stream to the Teensy as it arrives
    if (nvs_mgr_get_cert(NVS_KEY_ROOT_CA, root_ca, sizeof(root_ca)) != ESP_OK) return "no_root_ca";
    esp_http_client_config_t cfg = { .url = url, .cert_pem = root_ca, .timeout_ms = 15000, .buffer_size = 2048 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return "http_init_failed";
    const char *why = NULL;
    bool begun = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    if (esp_http_client_open(h, 0) != ESP_OK) { why = "download_failed"; snprintf(detail, dlen, "open"); goto out; }
    int64_t len = esp_http_client_fetch_headers(h);
    int code = esp_http_client_get_status_code(h);
    if (code != 200) { why = "download_failed"; snprintf(detail, dlen, "HTTP %d", code); goto out; }
    if (len <= 0 || len > TOTA_MAX_IMAGE || len % 4) { why = "bad_image_size"; snprintf(detail, dlen, "%lld bytes", (long long)len); goto out; }

    // From here the NILM data stops: the timeline marks it (start/end with the same ref)
    mqtt_app_publish_event("teensy_update", "start", m->version, previous);
    *started = true;
    led_set(LED_OTA);
    r = fxs_begin(&s, (uint32_t)len, new_maj, new_min);
    if (r != 0) { why = "teensy_refused_begin"; snprintf(detail, dlen, "%s (free flash %lu)", fxs_status_name(r), (unsigned long)s.buffer_size); goto out; }
    begun = true;
    ESP_LOGI(TAG, "Teensy %s -> %s: %lld bytes, free flash %lu", previous, m->version, (long long)len, (unsigned long)s.buffer_size);

    int64_t got = 0;
    while (got < len) {
        int n = esp_http_client_read(h, (char *)rbuf, sizeof(rbuf));
        if (n <= 0) { why = "download_failed"; snprintf(detail, dlen, "read at %lld", (long long)got); goto out; }
        mbedtls_sha256_update(&sha, rbuf, n);
        if ((r = fxs_data(&s, rbuf, (size_t)n)) != 0) { why = "teensy_write_failed"; snprintf(detail, dlen, "%s at %lu", fxs_status_name(r), (unsigned long)s.sent); goto out; }
        got += n;
    }

    // 3. The image must be the one the laptop signed, before the Teensy may install it
    unsigned char digest[32];
    char hex[65];
    mbedtls_sha256_finish(&sha, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(hex, m->sha256) != 0) { why = "sha256_mismatch"; snprintf(detail, dlen, "%.16s", hex); goto out; }

    if ((r = fxs_end(&s)) != 0) { why = "teensy_refused_image"; snprintf(detail, dlen, "%s", fxs_status_name(r)); goto out; }
    if ((r = fxs_commit(&s)) != 0) { why = "teensy_refused_commit"; snprintf(detail, dlen, "%s", fxs_status_name(r)); goto out; }
    begun = false;   // committed: the Teensy is rewriting its program and rebooting

    // 4. The new program must come back and report the new version
    {
        TickType_t t0 = xTaskGetTickCount();
        why = "not_running_new_version";
        snprintf(detail, dlen, "no data frame within %d s", TOTA_BOOT_WAIT_MS / 1000);
        while (pdTICKS_TO_MS(xTaskGetTickCount() - t0) < TOTA_BOOT_WAIT_MS) {
            uint8_t a, b; uint32_t age;
            if (nilm_bridge_teensy_fw(&a, &b, &age) && age < 1000 && pdTICKS_TO_MS(xTaskGetTickCount() - t0) > age + 500) {
                if (a == new_maj && b == new_min) { why = NULL; detail[0] = 0; break; }
                snprintf(detail, dlen, "reports v%u.%u.0", a, b);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

out:
    mbedtls_sha256_free(&sha);
    if (begun) {
        int a = fxs_abort(&s);         // the Teensy erases its buffer and reboots into the old program
        ESP_LOGW(TAG, "Abort sent to the Teensy: %s", fxs_status_name(a));
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    led_set(LED_OPERATIONAL);
    if (s.retries) ESP_LOGW(TAG, "%lu packet(s) retransmitted", (unsigned long)s.retries);
    return why;
}

void teensy_ota_run_pending(void)
{
    if (!__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE)) return;
    hota_manifest_t m = s_req.m;
    char url[256];
    strlcpy(url, s_req.url, sizeof(url));

    char detail[96] = "", previous[16] = "";
    char evt_detail[64];
    bool started = false;
    const char *why = run(&m, url, detail, sizeof(detail), previous, sizeof(previous), &started);
    if (why) {
        if (started) {
            snprintf(evt_detail, sizeof(evt_detail), "failed: %s", why);
            mqtt_app_publish_event("teensy_update", "end", m.version, evt_detail);
        }
        publish_failure(why, detail, m.version, previous);
    } else {
        snprintf(evt_detail, sizeof(evt_detail), "%s -> %s", previous, m.version);
        mqtt_app_publish_event("teensy_update", "end", m.version, evt_detail);
        char json[200];
        snprintf(json, sizeof(json), "{\"type\":\"teensy_ota_done\",\"version\":\"%s\",\"previous\":\"%s\",\"ts\":%lld}",
                 m.version, previous, (long long)time(NULL));
        mqtt_app_publish_status(json);
        ESP_LOGI(TAG, "Teensy now runs %s (was %s)", m.version, previous);
    }
    __atomic_store_n(&s_pending, false, __ATOMIC_RELEASE);
}
