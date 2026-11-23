// main/ntp_sync.h
#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ntp_sync_wait(uint32_t timeout_ms);