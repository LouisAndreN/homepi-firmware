// main/config.h
#pragma once

// --- Identification ----------------
#define NVS_KEY_DEVICE_ID   "device_id"

// --- Network: everything goes through nginx on 443 ----------------
#define HOMEPI_API_HOST     "homepi.internal"   // resolvable from the IoT subnet
// No port in the host: standard HTTPS 443 through nginx

// --- step-ca via Nginx ----------------------------------------
#define STEP_SIGN_URL       "https://homepi.internal/step/sign"
#define STEP_CA_AUDIENCE    "https://127.0.0.1:8443/1.0/sign#x5c/idevid-x5c"
#define STEP_ROOTS_URL      "https://homepi.internal/step/roots"

// --- Devices API through nginx (TLS terminated by nginx -> FastAPI :8090) ---
#define API_BASE_URL        "https://" HOMEPI_API_HOST
// Used in enrollment.c with snprintf or directly:
// https://homepi.internal/api/devices/announce
// https://homepi.internal/api/devices/{id}/status
// https://homepi.internal/api/devices/enroll
// https://homepi.internal/api/devices/x5c-enrolled

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
#define GPIO_BUTTON            21

// --- I2C ------------------------------------------------------
#define I2C_MASTER_SDA         6
#define I2C_MASTER_SCL         7
#define I2C_MASTER_FREQ_HZ     100000
#define I2C_SCD41_ADDR         0x62
#define I2C_BME688_ADDR        0x76
#define I2C_BH1750_ADDR        0x23
#define I2C_SHT40_ADDR         0x44
#define I2C_SPS30_ADDR         0x69
#define I2C_SGP41_ADDR         0x59

// --- MQTT topics ----------------------------------------------
#define MQTT_TOPIC_SENSORS  "iot/%s/sensors"
#define MQTT_TOPIC_NILM     "iot/%s/nilm" // NILM: dedicated topic, separate
                                                 // from the generic sensor stream
                                                 // (see Telegraf, measurement
                                                 // InfluxDB "nilm_readings")
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