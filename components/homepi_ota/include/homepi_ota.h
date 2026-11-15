// components/homepi_ota/include/homepi_ota.h
//
// Signed OTA for the HomePi firmware (v2.4.1, request queue v2.4.2). Shared by the sensor, LED and
// NILM projects.
//
// An OTA command carries a manifest and its signature:
//   manifest   {"chip_target":"esp32c3","project":"homepi_sensor","sha256":"<64 hex>","version":"v2.4.1"}
//   signature  base64 of the DER ECDSA P-256 signature of the manifest bytes (SHA-256)
// The signing key never leaves the operator's laptop; the public key is compiled in
// (ota_signing_pub.pem). The Pi only relays what was signed: it cannot sign an image.
//
// Checked before anything is written: the signature, the project (same as the running image), the
// chip target, a version strictly newer than the running one. Checked on the image: the version and
// the project in its app descriptor, and its SHA-256, before it is marked bootable. A refused
// command leaves the device on its current image and records the reason.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOTA_MANIFEST_MAX   384
#define HOTA_SIGNATURE_MAX  160

typedef enum {
    HOTA_EVENT_DOWNLOADING,
    HOTA_EVENT_FAILED,
} hota_event_t;

// Optional: the application maps events to its LED. Default: no-op (weak symbol).
void hota_on_event(hota_event_t event);

typedef struct {
    char project[32];
    char chip_target[16];
    char version[32];
    char sha256[65];
} hota_manifest_t;

// Checks the signed manifest against this device (signature, project, chip, newer version).
// Fills *out when not NULL. On refusal the reason is recorded (hota_take_last_failure).
esp_err_t hota_check_manifest(const char *manifest, const char *signature_b64, hota_manifest_t *out);

// Checks the manifest, downloads the image from url (server certificate checked against the root CA
// in NVS), checks it, installs it and restarts. Returns only on failure, with the reason recorded.
esp_err_t hota_start(const char *url, const char *manifest, const char *signature_b64);

// v2.4.2: queue a checked command for the application's main task. The MQTT task only checks and
// queues (static buffer, no allocation, no task creation); the main task, whose stack is reserved at
// boot, stops MQTT and runs the download. v2.4.0 and v2.4.1 created an 8 KB task per command and
// ignored the result: on the device with the least free heap the command vanished without a report.
// Returns ESP_OK when queued (*out filled when not NULL), or the refusal, recorded like any other.
esp_err_t hota_request(const char *url, const char *manifest, const char *signature_b64, hota_manifest_t *out);

// True when a request is waiting for hota_run_request().
bool hota_has_request(void);

// Runs the queued request (hota_start). Call from the main task with MQTT stopped. Restarts on
// success; returns the error otherwise, with the reason recorded, and the queue empty.
esp_err_t hota_run_request(void);

// JSON of the acknowledgment of a queued request ({"type":"ota_received","version":...}): lets the Pi
// tell a command that never arrived from one that failed. Built from the queued request.
bool hota_received_json(char *json, size_t len);

// JSON of the last refusal or failure ({"type":"ota_failed","reason":...}); false when there is none.
// Clears it, so a failure is published once.
bool hota_take_last_failure(char *json, size_t len);

// Compares firmware versions vMAJOR.MINOR.PATCH[-rcN] (a release is newer than its release candidates).
// Sets *ok to false when either version cannot be parsed (then the result is 0).
int hota_version_cmp(const char *a, const char *b, bool *ok);

#ifdef __cplusplus
}
#endif
