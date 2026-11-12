// components/homepi_identity/include/homepi_identity.h
//
// Device identity shared by the sensor, LED and NILM firmware (v2.4.0).
//
//   IDevID  factory identity, flashed once with the device id; used only to obtain a first LDevID
//   LDevID  operational identity (365 days), used for MQTT mTLS and for registration
//
// Lifecycle:
//   1. No usable LDevID (first boot, or expired after a long time offline): hid_enroll() signs an X5C
//      token with the IDevID and asks step-ca for an LDevID (/step/sign). Nothing else re-enrolls:
//      a broker outage or a refused MQTT connection never wipes the LDevID (until v2.3.x, three MQTT
//      disconnects did, which re-issued a certificate at every Pi reboot and let a revoked device
//      come back with its IDevID).
//   2. hid_register() tells the registry which LDevID the device holds, presenting that LDevID as the
//      TLS client certificate (the API reads fingerprint and expiry from it). The registry decides:
//      pre-announced devices become active, unknown ones wait for an operator.
//   3. hid_renew_if_due() renews the LDevID directly with step-ca (/step/renew) when less than
//      30 days are left, with a token signed by the current LDevID key: same key, new certificate.
//
// The stored LDevID is the chain step-ca returns (leaf, then intermediate): renewal tokens must carry
// the intermediate, and an LDevID stored by v2.3.x (leaf only) gets it from /step/intermediates.pem.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_EVENT_ENROLLING,    // asking step-ca for a first LDevID
    HID_EVENT_RENEWING,     // renewing the LDevID
    HID_EVENT_FAILED,       // the last operation failed
} hid_event_t;

// Optional: the application maps events to its LED. Default: no-op (weak symbol).
void hid_on_event(hid_event_t event);

// What the device tells the registry. NULL fields are read from NVS ("hw_model", "capabilities"
// as comma-separated values), then fall back to "unknown".
typedef struct {
    const char *hw_model;
    const char *capabilities_csv;
} hid_profile_t;

// True when an LDevID is stored, parses, matches its private key and is not expired (the expiry is
// checked only once the clock is set: before NTP an LDevID is assumed valid).
bool hid_ldevid_usable(void);

// Days until the LDevID expires (negative when expired); ESP_ERR_INVALID_STATE before NTP.
esp_err_t hid_ldevid_days_left(int *days);

// First enrollment with the IDevID (X5C provisioner). Stores the LDevID; the caller restarts.
esp_err_t hid_enroll(void);

// Registration with the LDevID as client certificate. ESP_OK when the registry answers active;
// ESP_ERR_NOT_ALLOWED when it refuses (pending or suspended); ESP_FAIL on transport errors.
esp_err_t hid_register(const hid_profile_t *profile);

// Renewal with step-ca when fewer than 30 days are left (force=true renews now, for tests).
// Sets *renewed; on success the caller re-registers and reconnects MQTT with the new certificate.
esp_err_t hid_renew_if_due(bool force, bool *renewed);

#ifdef __cplusplus
}
#endif
