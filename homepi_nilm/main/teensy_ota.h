// main/teensy_ota.h: remote update of the Teensy 4.0 through this bridge (v2.6.0).
//
// Command on iot/<id>/commands (published by the Pi's rollout, never typed by hand):
//   {"type":"teensy_ota","url":"https://homepi.internal/fw/homepi_nilm_teensy/v3.1.0.bin",
//    "manifest":"{\"chip_target\":\"teensy40\",\"project\":\"homepi_nilm_teensy\",...}",
//    "signature":"<base64 DER ECDSA P-256>"}
// Same trust chain as the ESP32 images: the manifest is signed on the operator's laptop with the
// OTA key, checked here against the public key compiled into this image, and the SHA-256 of the
// downloaded image must match it before the Teensy is told to install it (COMMIT). The Pi only
// relays: it cannot make the Teensy run an image the laptop did not sign.
//
// Reports on iot/<id>/status:
//   {"type":"teensy_ota_received","version":...}            command checked and queued
//   {"type":"teensy_ota_done","version":...,"previous":...}  the Teensy runs the new version
//   {"type":"teensy_ota_failed","reason":...,"detail":...,"version":...,"running":...}
// and the interruption of the NILM data on iot/<id>/event (the dashboard timeline marks it):
//   {"kind":"teensy_update","phase":"start"|"end","ref":<version>,"detail":...}
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// MQTT task: checks the signature, the project (homepi_nilm_teensy), the chip (teensy40) and the URL,
// then queues the command for the main task. Publishes received or failed.
void teensy_ota_handle_command(const char *url, const char *manifest, const char *signature);

// Main task: runs a queued update (10-30 s; MQTT stays connected). No-op when nothing is queued.
void teensy_ota_run_pending(void);
