// main/nilm_bridge.c
#include "nilm_bridge.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "fxproto.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "nilm_bridge";

// ── UART configuration ──────────────────────────────────────────
// The Teensy sends at 921600 baud (Serial1.begin(921600)): keep in sync.
#define NILM_UART_PORT      UART_NUM_1
#define NILM_UART_BAUD      921600
#define NILM_UART_RX_PIN    44
// v2.6.0: TX to the Teensy (Serial1 RX, pin 0) for the remote update, XIAO D4 = GPIO5, series
// resistor. Not GPIO43: it is the UART0 console TX, and the ROM prints its boot text there at every
// reset whatever the configuration (only an eFuse stops it); the wire was moved from GPIO43 to D4 so
// that nothing but update packets reaches the Teensy. The Teensy still ignores anything that does not
// parse (fxproto.h).
#define NILM_UART_TX_PIN    5
#define NILM_UART_RX_BUF_SZ 4096                 // ~60 frames/s x 68 B: ~1 s of margin
#define FS_HZ               44100.0f

// CRC8: bit-for-bit identical to crc8() of the Teensy firmware (poly 0x31, init 0xFF).
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
    return crc;
}

// ── Window accumulator (protected by s_mutex) ──────────────────
typedef struct {
    uint32_t cycles;
    double   sV2, sI12, sI22;          // sums of squares of the per-cycle RMS
    double   sP1, sQ1, sP2, sQ2;
    double   sTHD, sH3, sH5, sH7, sH9;
    double   sN;                       // sum of n_samples
    uint32_t events, glitches;
    int8_t   last_event;
    float    event_dS;
    uint8_t  fw_major, fw_minor;
    uint32_t teensy_ts_ms;
} accum_t;

static accum_t           s_acc;
static uint32_t          s_frames_ok, s_frames_crc_err, s_frames_bad_len;
static SemaphoreHandle_t s_mutex = NULL;

// Teensy firmware of the last data frame, and when it arrived (remote update: before / after)
static uint8_t           s_fw_major, s_fw_minor;
static TickType_t        s_last_frame_tick;
static bool              s_seen_frame;

// Replies of the Teensy to remote-update packets (frame type 0x04), read by teensy_ota.c
static QueueHandle_t     s_replies = NULL;

static void parse_frame(const uint8_t *p, nilm_frame_t *f)
{
    int i = 0;
#define RD(dst, sz) do { memcpy(&(dst), p + i, sz); i += sz; } while (0)
    f->fw_major = p[i++];
    f->fw_minor = p[i++];
    RD(f->V_n, 4);  RD(f->I1_n, 4); RD(f->I2_n, 4);
    RD(f->P1_n, 4); RD(f->Q1_n, 4); RD(f->P2_n, 4); RD(f->Q2_n, 4);
    RD(f->THD, 4);  RD(f->H3, 4);   RD(f->H5, 4);  RD(f->H7, 4); RD(f->H9, 4);
    f->event_flag = (int8_t)p[i++];
    RD(f->dS_n, 4);
    RD(f->teensy_ts_ms, 4);
    RD(f->n_samples, 2);
    RD(f->glitch_count, 2);
#undef RD
    // i == NILM_FRAME_PAYLOAD_LEN (63) by construction of the layout above
}

static void accumulate(const nilm_frame_t *f)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    accum_t *a = &s_acc;
    a->cycles++;
    a->sV2  += (double)f->V_n * f->V_n;
    a->sI12 += (double)f->I1_n * f->I1_n;
    a->sI22 += (double)f->I2_n * f->I2_n;
    a->sP1 += f->P1_n; a->sQ1 += f->Q1_n; a->sP2 += f->P2_n; a->sQ2 += f->Q2_n;
    a->sTHD += f->THD; a->sH3 += f->H3; a->sH5 += f->H5; a->sH7 += f->H7; a->sH9 += f->H9;
    a->sN += f->n_samples;
    a->glitches += f->glitch_count;
    if (f->event_flag != 0) {
        a->events++;
        if (a->last_event == 0 || fabsf(f->dS_n) > fabsf(a->event_dS)) {
            a->last_event = f->event_flag;
            a->event_dS   = f->dS_n;
        }
    }
    a->fw_major = f->fw_major; a->fw_minor = f->fw_minor;
    a->teensy_ts_ms = f->teensy_ts_ms;
    s_frames_ok++;
    s_fw_major = f->fw_major; s_fw_minor = f->fw_minor;
    s_last_frame_tick = xTaskGetTickCount(); s_seen_frame = true;
    xSemaphoreGive(s_mutex);
}

// ── UART read task: resynchronises on the header at every byte ─────
static void uart_rx_task(void *arg)
{
    (void)arg;
    enum { WAIT_AA, WAIT_55, WAIT_TYPE, WAIT_LEN, READ_PAYLOAD, READ_CRC } state = WAIT_AA;
    uint8_t buf[NILM_FRAME_PAYLOAD_LEN];
    uint8_t type = 0, want = 0;     // type 0x03 (data, 63 B) or 0x04 (update reply, 8 B)
    int pos = 0;
    uint8_t byte;
    nilm_frame_t f;

    while (1) {
        if (uart_read_bytes(NILM_UART_PORT, &byte, 1, pdMS_TO_TICKS(1000)) <= 0) continue;
        switch (state) {
            case WAIT_AA:   if (byte == 0xAA) state = WAIT_55; break;
            case WAIT_55:   state = (byte == 0x55) ? WAIT_TYPE : WAIT_AA; break;
            case WAIT_TYPE:
                if (byte == NILM_FRAME_TYPE || byte == FXP_REPLY_TYPE) {
                    type = byte;
                    want = (byte == NILM_FRAME_TYPE) ? NILM_FRAME_PAYLOAD_LEN : FXP_REPLY_LEN;
                    state = WAIT_LEN;
                } else {
                    if (byte == 0x01) {   // v2 Teensy firmware still flashed
                        xSemaphoreTake(s_mutex, portMAX_DELAY); s_frames_bad_len++; xSemaphoreGive(s_mutex);
                    }
                    state = WAIT_AA;
                }
                break;
            case WAIT_LEN:
                if (byte != want) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY); s_frames_bad_len++; xSemaphoreGive(s_mutex);
                    state = WAIT_AA;
                } else { pos = 0; state = READ_PAYLOAD; }
                break;
            case READ_PAYLOAD:
                buf[pos++] = byte;
                if (pos >= want) state = READ_CRC;
                break;
            case READ_CRC:
                if (byte == crc8(buf, want)) {
                    if (type == NILM_FRAME_TYPE) {
                        parse_frame(buf, &f);
                        accumulate(&f);
                    } else {
                        xQueueSend(s_replies, buf, 0);   // full queue: the sender times out and retries
                    }
                } else {
                    xSemaphoreTake(s_mutex, portMAX_DELAY); s_frames_crc_err++; xSemaphoreGive(s_mutex);
                }
                state = WAIT_AA;
                break;
        }
    }
}

esp_err_t nilm_bridge_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_replies = xQueueCreate(8, FXP_REPLY_LEN);
    if (!s_mutex || !s_replies) return ESP_ERR_NO_MEM;
    memset(&s_acc, 0, sizeof(s_acc));

    uart_config_t cfg = {
        .baud_rate = NILM_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(NILM_UART_PORT, NILM_UART_RX_BUF_SZ, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;
    if ((ret = uart_param_config(NILM_UART_PORT, &cfg)) != ESP_OK) return ret;
    if ((ret = uart_set_pin(NILM_UART_PORT, NILM_UART_TX_PIN, NILM_UART_RX_PIN,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) != ESP_OK) return ret;
    if (xTaskCreate(uart_rx_task, "nilm_uart_rx", 4096, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "NILM bridge v3 (raw frames, window means) on UART%d %d baud rx=%d tx=%d",
             NILM_UART_PORT, NILM_UART_BAUD, NILM_UART_RX_PIN, NILM_UART_TX_PIN);
    return ESP_OK;
}

bool nilm_bridge_has_pending_event(void)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ev = s_acc.events > 0;
    xSemaphoreGive(s_mutex);
    return ev;
}

// Takes the current window and starts a new one, atomically.
static bool take_window(nilm_window_t *w)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    accum_t a = s_acc;
    memset(&s_acc, 0, sizeof(s_acc));
    w->frames_ok = s_frames_ok; w->frames_crc_err = s_frames_crc_err; w->frames_bad_len = s_frames_bad_len;
    xSemaphoreGive(s_mutex);

    if (a.cycles == 0) return false;
    double n = a.cycles;
    w->cycles = a.cycles;
    w->fw_major = a.fw_major; w->fw_minor = a.fw_minor;
    w->V_n  = (float)sqrt(a.sV2 / n);
    w->I1_n = (float)sqrt(a.sI12 / n);
    w->I2_n = (float)sqrt(a.sI22 / n);
    w->P1_n = (float)(a.sP1 / n); w->Q1_n = (float)(a.sQ1 / n);
    w->P2_n = (float)(a.sP2 / n); w->Q2_n = (float)(a.sQ2 / n);
    w->THD = (float)(a.sTHD / n); w->H3 = (float)(a.sH3 / n); w->H5 = (float)(a.sH5 / n);
    w->H7 = (float)(a.sH7 / n);   w->H9 = (float)(a.sH9 / n);
    w->f_hz = (float)(FS_HZ / (a.sN / n));
    w->event_flag = a.last_event; w->dS_n = a.event_dS; w->events = a.events;
    w->glitches = a.glitches; w->teensy_ts_ms = a.teensy_ts_ms;
    return true;
}

esp_err_t nilm_bridge_publish(esp_mqtt_client_handle_t client,
                              const char *device_id,
                              const char *topic_nilm)
{
    (void)device_id;   // the topic already carries it; kept for the v2 call signature
    nilm_window_t w;
    if (!take_window(&w)) {
        ESP_LOGW(TAG, "No Teensy frame in this window (ok=%lu crc_err=%lu bad_len=%lu): nothing published",
                 (unsigned long)w.frames_ok, (unsigned long)w.frames_crc_err, (unsigned long)w.frames_bad_len);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "ts", (double)time(NULL));   // ESP32 wall clock (NTP)
    // Raw normalised values: full float precision (P_n can be ~1e-5 on a small load)
    cJSON_AddNumberToObject(j, "V_n",  w.V_n);
    cJSON_AddNumberToObject(j, "I1_n", w.I1_n);
    cJSON_AddNumberToObject(j, "I2_n", w.I2_n);
    cJSON_AddNumberToObject(j, "P1_n", w.P1_n);
    cJSON_AddNumberToObject(j, "Q1_n", w.Q1_n);
    cJSON_AddNumberToObject(j, "P2_n", w.P2_n);
    cJSON_AddNumberToObject(j, "Q2_n", w.Q2_n);
    cJSON_AddNumberToObject(j, "THD", roundf(w.THD * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(j, "H3",  roundf(w.H3 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(j, "H5",  roundf(w.H5 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(j, "H7",  roundf(w.H7 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(j, "H9",  roundf(w.H9 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(j, "f_hz", roundf(w.f_hz * 100.0f) / 100.0f);
    cJSON_AddNumberToObject(j, "cycles", w.cycles);
    cJSON_AddNumberToObject(j, "event_flag", w.event_flag);
    cJSON_AddNumberToObject(j, "dS_n", w.dS_n);
    cJSON_AddNumberToObject(j, "events", w.events);
    cJSON_AddNumberToObject(j, "glitches", w.glitches);
    cJSON_AddNumberToObject(j, "teensy_ts_ms", (double)w.teensy_ts_ms);
    // Numeric so Telegraf stores it (string fields are dropped): 300 = v3.0
    cJSON_AddNumberToObject(j, "fw_teensy", w.fw_major * 100 + w.fw_minor);
    cJSON_AddNumberToObject(j, "frames_ok", (double)w.frames_ok);
    cJSON_AddNumberToObject(j, "frames_crc_err", (double)w.frames_crc_err);
    cJSON_AddNumberToObject(j, "frames_bad_len", (double)w.frames_bad_len);

    wifi_ap_record_t ap;
    cJSON_AddNumberToObject(j, "rssi", (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0);
    cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
    char hw_model[32] = {0};
    nvs_mgr_get_string("hw_model", hw_model, sizeof(hw_model));
    cJSON_AddStringToObject(j, "hw_model", hw_model[0] ? hw_model : "xiao_esp32s3");

    char *s = cJSON_PrintUnformatted(j);
    int msg_id = esp_mqtt_client_publish(client, topic_nilm, s, 0, /*qos*/1, /*retain*/0);
    ESP_LOGI(TAG, "Published msg_id=%d cycles=%lu P1_n=%+.6f Q1_n=%+.6f ev=%d (ok=%lu crc_err=%lu)",
             msg_id, (unsigned long)w.cycles, w.P1_n, w.Q1_n, w.event_flag,
             (unsigned long)w.frames_ok, (unsigned long)w.frames_crc_err);
    free(s);
    cJSON_Delete(j);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

// ── Remote update of the Teensy (v2.6.0) ───────────────────────

esp_err_t nilm_bridge_write(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(NILM_UART_PORT, (const char *)data, len);
    if (n != (int)len) return ESP_FAIL;
    return uart_wait_tx_done(NILM_UART_PORT, pdMS_TO_TICKS(200));
}

void nilm_bridge_flush_replies(void)
{
    uint8_t r[FXP_REPLY_LEN];
    while (xQueueReceive(s_replies, r, 0) == pdTRUE) { }
}

bool nilm_bridge_wait_reply(uint16_t seq, uint8_t cmd, uint32_t timeout_ms, uint8_t *status, uint32_t *value)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t r[FXP_REPLY_LEN];
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(end - now) <= 0) return false;
        if (xQueueReceive(s_replies, r, end - now) != pdTRUE) return false;
        // A late reply to an earlier attempt is skipped: only (seq, cmd) of this packet counts
        if (fxp_get16(r) != seq || r[2] != cmd) continue;
        *status = r[3];
        *value  = fxp_get32(r + 4);
        return true;
    }
}

bool nilm_bridge_teensy_fw(uint8_t *major, uint8_t *minor, uint32_t *age_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool seen = s_seen_frame;
    *major = s_fw_major; *minor = s_fw_minor;
    *age_ms = seen ? (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_last_frame_tick) : UINT32_MAX;
    xSemaphoreGive(s_mutex);
    return seen;
}
