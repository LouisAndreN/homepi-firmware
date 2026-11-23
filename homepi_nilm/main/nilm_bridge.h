// main/nilm_bridge.h
//
// UART -> MQTT bridge for the NILM board (Teensy 4.0 -> XIAO ESP32-S3).
// Reuses the existing enrolment/mTLS/MQTT infrastructure as it is
// (enrollment.c, mqtt_app.c, nvs_manager.c); this module ONLY:
//   1. Reads continuously the binary frames sent by the Teensy over UART
//      (protocol defined on the Teensy side in send_frame(), teensy_nilm_phase2_*.ino)
//   2. Parses them and keeps them in cache (last known value)
//   3. Exposes an MQTT publish function following exactly the same
//      convention as sensors_publish() (flat JSON, topic iot/%s/sensors)
//
// This device provisions itself as a device in its own right (its own
// device_id, son propre enrollment zero-touch) via provision_device.sh /
// flash_device.sh existants — aucune modification de ces scripts requise.
#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <stdbool.h>

// ── Teensy frame format (see send_frame() in the NILM firmware) ──
// Header : 0xAA 0x55 0x01 <len>
// Payload (62 bytes, little-endian, as written by the Teensy):
//   float    V_rms
//   float    I1_rms
//   float    I2_rms
//   float    P_L1
//   float    P_L2
//   float    PF_L1
//   float    THD
//   float    H3
//   float    H5
//   float    H7
//   float    H9
//   int8_t   event_flag
//   float    delta_P
//   uint32_t ts              (millis() on the Teensy: local clock, NOT wall clock)
//   float    lag_confirmed   (v2.2: lag value actually used for
//                             the feature computations, validated by consensus)
//   float    last_prominence (v2.2: sharpness of the last correlation peak
//                             tried, accepted or not; diagnostics)
//   uint8_t  consensus_count (v2.2: 0-3, filling of the consensus window
//                             when the frame is sent)
// Followed by a CRC8 (poly 0x31, init 0xFF, identical to the crc8() function
// of the Teensy firmware; never let the two sides diverge).
// IMPORTANT: this format MUST stay in sync with send_frame() on the
// Teensy side; any change to one without the other silently invalidates
// every frame (rejected as "unexpected length").
#define NILM_FRAME_HEADER_LEN   4
#define NILM_FRAME_PAYLOAD_LEN  62
#define NILM_FRAME_CRC_LEN      1
#define NILM_FRAME_TOTAL_LEN    (NILM_FRAME_HEADER_LEN + NILM_FRAME_PAYLOAD_LEN + NILM_FRAME_CRC_LEN)

typedef struct {
    float    V_rms, I1_rms, I2_rms;
    float    P_L1, P_L2, PF_L1;
    float    THD, H3, H5, H7, H9;
    int8_t   event_flag;
    float    delta_P;
    uint32_t teensy_ts_ms;   // local Teensy clock, not the ESP32 wall clock
    float    lag_confirmed;    // v2.2
    float    last_prominence;  // v2.2
    uint8_t  consensus_count;  // v2.2 — 0-3
    bool     valid;          // false until a valid frame has been received
    uint32_t frames_ok;      // compteur diagnostic
    uint32_t frames_crc_err; // compteur diagnostic
} nilm_reading_t;

// Initialises the UART (RX from Teensy Serial1 TX) and starts the
// read/parse task in the background. Call once at boot, like
// sensors_init(); independent of the Wi-Fi/enrolment/MQTT state.
esp_err_t nilm_bridge_init(void);

// Copies the last valid reading into *out. Returns ESP_ERR_INVALID_STATE
// if no frame has been received since boot.
esp_err_t nilm_bridge_get_latest(nilm_reading_t *out);

// Publishes the last reading on the device's MQTT sensors topic, with
// exactly the same flat JSON convention as sensors_publish() (see
// sensors.c). Call from the main loop, like
// mqtt_app_publish_sensors().
esp_err_t nilm_bridge_publish(esp_mqtt_client_handle_t client,
                               const char *device_id,
                               const char *topic_sensors);

// true if a load event (event_flag != 0) is present in the
// last frame received: lets the main loop trigger an
// immediate publish rather than wait for the next periodic tick
// (a load event must not wait up to 30 s to appear
// on the dashboard).
bool nilm_bridge_has_pending_event(void);