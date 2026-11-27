// main/nilm_bridge.h
//
// UART -> MQTT bridge for the NILM board (Teensy 4.0 -> XIAO ESP32-S3).
// Reuses the existing enrolment/mTLS/MQTT infrastructure as it is
// (homepi_identity, mqtt_app.c, nvs_manager.c); this module ONLY:
//   1. Reads the binary frames sent by the Teensy over UART, one per mains
//      cycle (protocol: build_frame_v3() in teensy_nilm_phase2_v3_0.ino)
//   2. ACCUMULATES every valid cycle between two publishes
//   3. Publishes the window means on iot/<device_id>/nilm
//
// Frame v3 carries RAW NORMALISED values (ADC full-scale units). No scale,
// polarity or phase is applied on the Teensy or here: the Pi calibrates
// (nilm_calibration.json, applied by the homepi-nilm-calibrate processor).
// The published field names end in _n so raw values can never be mistaken
// for the calibrated fields of firmware v2.x (V_rms, P_L1, ...).
//
// Why accumulate: the Teensy sends ~60 frames/s and the bridge publishes
// every 1.5 s. v2.x published the LAST frame only, so one random cycle out
// of ~90 stood for the whole window. The window mean of P_n and Q_n is
// exactly the mean power of the window, which is what energy needs.
#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <stdbool.h>

// ── Teensy frame v3 (see build_frame_v3() in the Teensy firmware) ──
// Header: 0xAA 0x55 0x03 <len=63>, payload little-endian, then CRC8
// (poly 0x31, init 0xFF, identical to crc8() on the Teensy).
//  off  type  field
//   0   u8    fw_major
//   1   u8    fw_minor
//   2   f32   V_n
//   6   f32   I1_n
//  10   f32   I2_n
//  14   f32   P1_n
//  18   f32   Q1_n
//  22   f32   P2_n
//  26   f32   Q2_n
//  30   f32   THD    (% of I1)
//  34   f32   H3     (/H1, I1)
//  38   f32   H5
//  42   f32   H7
//  46   f32   H9
//  50   i8    event_flag (+1 on, -1 off, 0)
//  51   f32   dS_n
//  55   u32   ts (Teensy millis, local clock)
//  59   u16   n_samples (f = 44100 / n)
//  61   u16   glitch_count
// IMPORTANT: this layout MUST stay in sync with build_frame_v3(). The frame
// type byte changed from 0x01 to 0x03 so that a v2 bridge rejects v3 frames
// and this bridge rejects v2 frames, instead of misreading them.
#define NILM_FRAME_TYPE         0x03
#define NILM_FRAME_HEADER_LEN   4
#define NILM_FRAME_PAYLOAD_LEN  63
#define NILM_FRAME_CRC_LEN      1

typedef struct {
    uint8_t  fw_major, fw_minor;
    float    V_n, I1_n, I2_n;
    float    P1_n, Q1_n, P2_n, Q2_n;
    float    THD, H3, H5, H7, H9;
    int8_t   event_flag;
    float    dS_n;
    uint32_t teensy_ts_ms;
    uint16_t n_samples;
    uint16_t glitch_count;
} nilm_frame_t;

// Window aggregate published to MQTT
typedef struct {
    uint32_t cycles;              // valid frames in the window
    uint8_t  fw_major, fw_minor;  // Teensy firmware of the last frame
    float    V_n, I1_n, I2_n;     // RMS over the window (sqrt of mean square)
    float    P1_n, Q1_n, P2_n, Q2_n;   // window means
    float    THD, H3, H5, H7, H9;      // window means
    float    f_hz;                // mains frequency from the mean cycle length
    int8_t   event_flag;          // last non-zero flag in the window
    float    dS_n;                // dS of that event (largest |dS| if several)
    uint32_t events;              // events in the window
    uint32_t glitches;            // despiked samples in the window
    uint32_t teensy_ts_ms;        // of the last frame
    uint32_t frames_ok;           // since boot
    uint32_t frames_crc_err;      // since boot
    uint32_t frames_bad_len;      // since boot (wrong type or length: v2 Teensy?)
} nilm_window_t;

// Initialises the UART (RX from Teensy Serial1 TX) and starts the read/parse
// task. Call once at boot; independent of the Wi-Fi/enrolment/MQTT state.
esp_err_t nilm_bridge_init(void);

// true if at least one load event arrived since the last publish: the main
// loop publishes immediately on the rising edge instead of waiting.
bool nilm_bridge_has_pending_event(void);

// Publishes the window aggregate on topic_nilm and starts a new window.
// Returns ESP_ERR_INVALID_STATE when no frame arrived in the window
// (nothing is published: a gap must stay a gap on the Pi).
esp_err_t nilm_bridge_publish(esp_mqtt_client_handle_t client,
                              const char *device_id,
                              const char *topic_nilm);
