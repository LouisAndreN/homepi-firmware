// main/sensors.h
#pragma once
#include "esp_err.h"
#include "mqtt_client.h"
#include <stdint.h>

typedef struct {
    float    temperature;
    float    humidity;
    float    co2;
    float    pressure;
    float    lux;
    float    iaq;
    float    pm1_0;
    float    pm2_5;
    float    pm4_0;
    float    pm10;
    uint16_t voc_raw;   // raw SRAW_VOC: NOT an index usable as it is
    uint16_t nox_raw;   // SRAW_NOX brut — idem
    int8_t   rssi;
    // Presence flags
    bool     scd41_ok;
    bool     bme688_ok;
    bool     bh1750_ok;
    bool     sht40_ok;
    bool     sps30_ok;
    bool     sgp41_ok;
} sensor_data_t;

typedef struct {
    uint8_t     addr;
    const char *name;
} i2c_probe_t;

esp_err_t sensors_init(void);
esp_err_t sensors_read(sensor_data_t *data);
esp_err_t sensors_publish(esp_mqtt_client_handle_t client,
                           const char *device_id,
                           const char *topic);

// Discovery + init combined: the ONLY function to call at boot
esp_err_t sensors_discover_and_init(void);

// Read all active sensors
esp_err_t sensors_read_all(sensor_data_t *out);

// Discovered capabilities (for the enrolment payload)
// ex: "co2,temperature,humidity,lux"
const char *sensors_get_capabilities_csv(void);

// derived hw_model (e.g. "bh1750+scd41")
const char *sensors_get_hw_model(void);