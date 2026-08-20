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
    uint16_t voc_raw;   // SRAW_VOC brut — PAS un index utilisable tel quel
    uint16_t nox_raw;   // SRAW_NOX brut — idem
    int8_t   rssi;
    // Flags de présence
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

// Discovery + init combinés — SEULE fonction à appeler au boot
esp_err_t sensors_discover_and_init(void);

// Lecture de tous les capteurs actifs
esp_err_t sensors_read_all(sensor_data_t *out);

// Capabilities découvertes (pour enrollment payload)
// ex: "co2,temperature,humidity,lux"
const char *sensors_get_capabilities_csv(void);

// hw_model dérivé (ex: "bh1750+scd41")
const char *sensors_get_hw_model(void);