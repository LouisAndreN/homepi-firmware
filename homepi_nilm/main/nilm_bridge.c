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
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "nilm_bridge";

// ── Configuration UART ──────────────────────────────────────────
// Adjust to the real XIAO ESP32-S3 <-> Teensy Serial1 wiring.
// The Teensy sends at 921600 baud (see Serial1.begin(921600) in the
// NILM firmware): these two values must stay in sync.
#define NILM_UART_PORT      UART_NUM_1
#define NILM_UART_BAUD      921600
#define NILM_UART_RX_PIN    44   // RX GPIO of the XIAO ESP32-S3: TO CHECK
                                 // against the real pinout of the board (open item)
#define NILM_UART_TX_PIN    UART_PIN_NO_CHANGE // unused, Teensy->ESP32 only
#define NILM_UART_RX_BUF_SZ 2048

// CRC8: MUST stay bit-for-bit identical to the crc8() function of the
// Teensy firmware (polynomial 0x31, init 0xFF). Never let the two
// implementations diverge: a change on one side without the other would
// silently invalidate every frame (rejected as corrupted).
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

// ── Shared state (last valid reading) ──────────────────────
static nilm_reading_t s_latest = {0};
static SemaphoreHandle_t s_mutex = NULL;

// ── Parsing of a validated payload buffer (CRC already checked) ───────
static void parse_payload(const uint8_t *p)
{
    nilm_reading_t r = {0};
    int idx = 0;
    memcpy(&r.V_rms,  p + idx, 4); idx += 4;
    memcpy(&r.I1_rms, p + idx, 4); idx += 4;
    memcpy(&r.I2_rms, p + idx, 4); idx += 4;
    memcpy(&r.P_L1,   p + idx, 4); idx += 4;
    memcpy(&r.P_L2,   p + idx, 4); idx += 4;
    memcpy(&r.PF_L1,  p + idx, 4); idx += 4;
    memcpy(&r.THD,    p + idx, 4); idx += 4;
    memcpy(&r.H3,     p + idx, 4); idx += 4;
    memcpy(&r.H5,     p + idx, 4); idx += 4;
    memcpy(&r.H7,     p + idx, 4); idx += 4;
    memcpy(&r.H9,     p + idx, 4); idx += 4;
    r.event_flag = (int8_t)p[idx]; idx += 1;
    memcpy(&r.delta_P, p + idx, 4); idx += 4;
    memcpy(&r.teensy_ts_ms, p + idx, 4); idx += 4;
    // v2.2 : champs de diagnostic lag/consensus
    memcpy(&r.lag_confirmed,   p + idx, 4); idx += 4;
    memcpy(&r.last_prominence, p + idx, 4); idx += 4;
    r.consensus_count = p[idx]; idx += 1;
    // idx must equal NILM_FRAME_PAYLOAD_LEN (62) here; not checked at
    // run time to stay light, consistency guaranteed by construction
    // (constants computed once, see nilm_bridge.h).

    r.valid = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t ok_count  = s_latest.frames_ok + 1;
    uint32_t err_count = s_latest.frames_crc_err;
    r.frames_ok      = ok_count;
    r.frames_crc_err = err_count;
    s_latest = r;
    xSemaphoreGive(s_mutex);
}

// ── UART read task: state machine on the byte stream ─
// Trusts NO alignment assumption: resynchronises on
// the header (0xAA 0x55 0x01) at every byte until a complete and
// valid frame (correct CRC) has been found. This is needed because a
// restart of either device can cut the stream in the
// middle of a frame, and the resume point must be robust without
// intervention manuelle.
static void uart_rx_task(void *arg)
{
    enum { WAIT_0xAA, WAIT_0x55, WAIT_0x01, WAIT_LEN, READ_PAYLOAD, READ_CRC } state = WAIT_0xAA;
    uint8_t payload_buf[NILM_FRAME_PAYLOAD_LEN];
    int payload_pos = 0;
    uint8_t declared_len = 0;
    uint8_t byte;

    while (1) {
        int n = uart_read_bytes(NILM_UART_PORT, &byte, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue; // timeout: no data, loop again

        switch (state) {
            case WAIT_0xAA:
                if (byte == 0xAA) state = WAIT_0x55;
                break;
            case WAIT_0x55:
                state = (byte == 0x55) ? WAIT_0x01 : WAIT_0xAA;
                break;
            case WAIT_0x01:
                state = (byte == 0x01) ? WAIT_LEN : WAIT_0xAA;
                break;
            case WAIT_LEN:
                declared_len = byte;
                if (declared_len != NILM_FRAME_PAYLOAD_LEN) {
                    // Unexpected length -> probably a resynchronisation
                    // in the middle of a stream, not a real frame. Start
                    // again from zero rather than risk reading a misaligned
                    // payload.
                    ESP_LOGW(TAG, "Unexpected frame length: %d (expected %d)",
                             declared_len, NILM_FRAME_PAYLOAD_LEN);
                    state = WAIT_0xAA;
                } else {
                    payload_pos = 0;
                    state = READ_PAYLOAD;
                }
                break;
            case READ_PAYLOAD:
                payload_buf[payload_pos++] = byte;
                if (payload_pos >= NILM_FRAME_PAYLOAD_LEN) state = READ_CRC;
                break;
            case READ_CRC: {
                uint8_t expected = crc8(payload_buf, NILM_FRAME_PAYLOAD_LEN);
                if (byte == expected) {
                    parse_payload(payload_buf);
                } else {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_latest.frames_crc_err++;
                    xSemaphoreGive(s_mutex);
                    ESP_LOGW(TAG, "Invalid CRC (received=0x%02X expected=0x%02X): frame dropped",
                             byte, expected);
                }
                state = WAIT_0xAA;
                break;
            }
        }
    }
}

esp_err_t nilm_bridge_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    uart_config_t uart_cfg = {
        .baud_rate = NILM_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(NILM_UART_PORT, NILM_UART_RX_BUF_SZ, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;
    ret = uart_param_config(NILM_UART_PORT, &uart_cfg);
    if (ret != ESP_OK) return ret;
    ret = uart_set_pin(NILM_UART_PORT, NILM_UART_TX_PIN, NILM_UART_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    BaseType_t task_ret = xTaskCreate(uart_rx_task, "nilm_uart_rx", 4096, NULL, 6, NULL);
    if (task_ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "NILM bridge UART initialised (port=%d baud=%d rx_pin=%d)",
             NILM_UART_PORT, NILM_UART_BAUD, NILM_UART_RX_PIN);
    return ESP_OK;
}

esp_err_t nilm_bridge_get_latest(nilm_reading_t *out)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_latest;
    xSemaphoreGive(s_mutex);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool nilm_bridge_has_pending_event(void)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has_event = s_latest.valid && (s_latest.event_flag != 0);
    xSemaphoreGive(s_mutex);
    return has_event;
}

esp_err_t nilm_bridge_publish(esp_mqtt_client_handle_t client,
                               const char *device_id,
                               const char *topic_sensors)
{
    nilm_reading_t r;
    esp_err_t ret = nilm_bridge_get_latest(&r);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No Teensy frame received since boot: publish skipped");
        return ret;
    }

    cJSON *payload = cJSON_CreateObject();
    // ts = wall clock of the ESP32 (via NTP), NOT teensy_ts_ms (the Teensy's
    // local millis() clock, unrelated to real time). teensy_ts_ms is
    // kept separately for diagnostics/latency only.
    cJSON_AddNumberToObject(payload, "ts", (double)time(NULL));

    cJSON_AddNumberToObject(payload, "V_rms",  roundf(r.V_rms  * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "I1_rms", roundf(r.I1_rms * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "I2_rms", roundf(r.I2_rms * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "P_L1",   roundf(r.P_L1   * 10.0f)   / 10.0f);
    cJSON_AddNumberToObject(payload, "P_L2",   roundf(r.P_L2   * 10.0f)   / 10.0f);
    cJSON_AddNumberToObject(payload, "PF_L1",  roundf(r.PF_L1  * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "THD",    roundf(r.THD    * 10.0f)   / 10.0f);
    cJSON_AddNumberToObject(payload, "H3", roundf(r.H3 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "H5", roundf(r.H5 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "H7", roundf(r.H7 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "H9", roundf(r.H9 * 1000.0f) / 1000.0f);
    cJSON_AddNumberToObject(payload, "event_flag", r.event_flag);
    cJSON_AddNumberToObject(payload, "delta_P", roundf(r.delta_P * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(payload, "teensy_ts_ms", (double)r.teensy_ts_ms);
    // v2.2: lag/consensus diagnostics: tells a reliable PF
    // (lag recently confirmed, clear prominence, full window) from a PF
    // computed on a reference not yet validated, without plugging
    // a serial monitor into the Teensy.
    cJSON_AddNumberToObject(payload, "lag_confirmed",   roundf(r.lag_confirmed * 100.0f) / 100.0f);
    cJSON_AddNumberToObject(payload, "last_prominence", roundf(r.last_prominence * 100.0f) / 100.0f);
    cJSON_AddNumberToObject(payload, "consensus_count", r.consensus_count);
    cJSON_AddNumberToObject(payload, "frames_ok", (double)r.frames_ok);
    cJSON_AddNumberToObject(payload, "frames_crc_err", (double)r.frames_crc_err);

    // ── Generic fleet fields, same conventions as sensors.c ──
    wifi_ap_record_t ap_info;
    int rssi = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : 0;
    cJSON_AddNumberToObject(payload, "rssi", rssi);
    cJSON_AddStringToObject(payload, "fw", esp_app_get_description()->version);

    char hw_model[32] = {0};
    nvs_mgr_get_string("hw_model", hw_model, sizeof(hw_model));
    cJSON_AddStringToObject(payload, "hw_model", hw_model[0] ? hw_model : "xiao_esp32s3");

    char *str = cJSON_PrintUnformatted(payload);
    int msg_id = esp_mqtt_client_publish(client, topic_sensors, str, 0, /*qos*/1, /*retain*/0);
    ESP_LOGI(TAG, "Published (msg_id=%d, frames_ok=%lu, crc_err=%lu): P_L1=%.1fW PF=%.3f ev=%d",
             msg_id, (unsigned long)r.frames_ok, (unsigned long)r.frames_crc_err,
             r.P_L1, r.PF_L1, r.event_flag);

    free(str);
    cJSON_Delete(payload);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}