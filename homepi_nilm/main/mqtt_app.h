// main/mqtt_app.h
#pragma once
#include "esp_err.h"
#include "mqtt_client.h"

extern esp_mqtt_client_handle_t mqtt_handle;

esp_err_t mqtt_app_start(void);
esp_err_t mqtt_app_publish_nilm(void); // NILM : remplace publish_sensors,
                                        // ce device n'a pas de capteurs I2C
void      mqtt_app_stop(void);