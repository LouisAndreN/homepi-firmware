// main/data_buffer.h
#pragma once
#include "sensors.h"
#include "mqtt_client.h"
#include "esp_err.h"

esp_err_t data_buffer_init(void);
esp_err_t data_buffer_push(const sensor_data_t *data);
int       data_buffer_flush(esp_mqtt_client_handle_t client,
                             const char *device_id,
                             const char *topic);
int       data_buffer_count(void);