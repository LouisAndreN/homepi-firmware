// main/config.h
#pragma once

// --- Identification ----------------
#define NVS_KEY_DEVICE_ID   "device_id"

// --- Network: everything goes through nginx on 443 ----------------
#define HOMEPI_API_HOST     "homepi.internal"

// --- step-ca via Nginx ----------------------------------------
#define STEP_SIGN_URL       "https://homepi.internal/step/sign"
#define STEP_CA_AUDIENCE    "https://127.0.0.1:8443/1.0/sign#x5c/idevid-x5c"
#define STEP_ROOTS_URL      "https://homepi.internal/step/roots"

// --- API devices via Nginx ---
#define API_BASE_URL        "https://" HOMEPI_API_HOST

// --- Direct MQTT mTLS (not through nginx) -------------------------
#define MQTT_BROKER_IP      "192.168.200.1"
#define MQTT_BROKER_PORT    8883

// --- Timings --------------------------------------------------
#define NTP_TIMEOUT_MS          30000
#define X5C_TIMEOUT_MS          60000
#define FALLBACK_POLL_MS        5000
#define SENSOR_PUBLISH_INTERVAL 30000
#define CERT_RENEW_DAYS_BEFORE  30

// --- GPIO -----------------------------------------------------
#define GPIO_LED               4
#define GPIO_BUTTON            2

// --- LED strip (WS2811 via 74HCT245) --------------------------
#define GPIO_LED_STRIP          1   // to input A1 of the 74HCT245

// --- MQTT topics ----------------------------------------------
#define MQTT_TOPIC_SENSORS  "iot/%s/sensors"
#define MQTT_TOPIC_STATUS   "iot/%s/status"
#define MQTT_TOPIC_COMMANDS "iot/%s/commands"

// --- NVS ------------------------------------------------------
#define NVS_NS_CONFIG       "config"
#define NVS_NS_CERTS        "certs"
#define NVS_KEY_WIFI_SSID   "wifi_ssid"
#define NVS_KEY_WIFI_PASS   "wifi_pass"
#define NVS_KEY_ENROLLED    "enrolled"
#define NVS_KEY_LDEVID_SLOT "ldevid_slot"
#define NVS_KEY_FW_VERSION  "fw_version"
#define NVS_KEY_IDEVID_CERT "idevid_cert"
#define NVS_KEY_IDEVID_KEY  "idevid_key"
#define NVS_KEY_ROOT_CA     "root_ca"
#define NVS_KEY_LDEVID_CERT_A "ldevid_crt_a"
#define NVS_KEY_LDEVID_KEY_A  "ldevid_key_a"
#define NVS_KEY_LDEVID_CERT_B "ldevid_crt_b"
#define NVS_KEY_LDEVID_KEY_B  "ldevid_key_b"