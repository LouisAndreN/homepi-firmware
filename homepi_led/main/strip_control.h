#pragma once
#include "esp_err.h"
#include <stdint.h>
#include "cJSON.h"

#define STRIP_NUM_LEDS   50
#define STRIP_MAX_LAYERS 6

esp_err_t strip_control_init(void);
void strip_control_apply_composition(cJSON *layers_json);
void strip_control_task_start(void);