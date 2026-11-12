// components/homepi_identity/src/identity_config.h
// Constants of the device identity, identical for every HomePi firmware (private to the component).
#pragma once

#define HID_API_HOST            "homepi.internal"
#define HID_REGISTER_URL        "https://" HID_API_HOST "/api/devices/x5c-enrolled"

// step-ca behind nginx (/step/sign, /step/renew, /step/intermediates.pem -> 127.0.0.1:8443)
#define HID_STEP_SIGN_URL           "https://" HID_API_HOST "/step/sign"
#define HID_STEP_RENEW_URL          "https://" HID_API_HOST "/step/renew"
#define HID_STEP_INTERMEDIATES_URL  "https://" HID_API_HOST "/step/intermediates.pem"
// Audiences step-ca accepts: the X5C provisioner for enrollment, the renew endpoint for renewal
#define HID_SIGN_AUDIENCE       "https://127.0.0.1:8443/1.0/sign#x5c/idevid-x5c"
#define HID_RENEW_AUDIENCE      "https://homepi.internal/1.0/renew"
#define HID_RENEW_ISSUER        "step-ca-client/1.0"

#define HID_HTTP_TIMEOUT_MS     60000
// Renew when less than this is left (the LDevID lives 365 days)
#define HID_RENEW_DAYS_BEFORE   30

// NVS (same keys as firmware v2.3.x: an existing identity is kept across the update)
#define NVS_NS_CONFIG           "config"
#define NVS_NS_CERTS            "certs"
#define NVS_KEY_DEVICE_ID       "device_id"
#define NVS_KEY_ROOT_CA         "root_ca"
#define NVS_KEY_IDEVID_CERT     "idevid_cert"
#define NVS_KEY_IDEVID_KEY      "idevid_key"
#define NVS_KEY_LDEVID_CERT_A   "ldevid_crt_a"
#define NVS_KEY_LDEVID_KEY_A    "ldevid_key_a"
#define NVS_KEY_LDEVID_CERT_B   "ldevid_crt_b"
#define NVS_KEY_LDEVID_KEY_B    "ldevid_key_b"
#define NVS_KEY_LDEVID_SLOT     "ldevid_slot"
