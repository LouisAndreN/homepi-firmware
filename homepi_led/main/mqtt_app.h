// main/mqtt_app.h
#pragma once
#include "esp_err.h"
#include "mqtt_client.h"

extern esp_mqtt_client_handle_t mqtt_handle;

esp_err_t mqtt_app_start(void);
esp_err_t mqtt_app_publish_sensors(void);
void      mqtt_app_stop(void);
// Runs a queued OTA command (v2.4.2). Call from the main loop: returns at once when none is queued.
void      mqtt_app_run_pending_ota(void);
void      mqtt_app_publish_heartbeat(void);