// main/mqtt_app.h
#pragma once
#include "esp_err.h"
#include "mqtt_client.h"

extern esp_mqtt_client_handle_t mqtt_handle;

esp_err_t mqtt_app_start(void);
esp_err_t mqtt_app_publish_nilm(void); // NILM : remplace publish_sensors,
                                        // this device has no I2C sensors
void      mqtt_app_stop(void);
// Runs a queued OTA command (v2.4.2). Call from the main loop: returns at once when none is queued.
void      mqtt_app_run_pending_ota(void);
// v2.6.0: status JSON on iot/<id>/status, events on iot/<id>/event ({kind, phase, ref, detail})
void      mqtt_app_publish_status(const char *json);
void      mqtt_app_publish_event(const char *kind, const char *phase, const char *ref, const char *detail);