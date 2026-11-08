// main/enrollment.h
#pragma once
#include "esp_err.h"

// Zero-touch : IDevID -> step-ca -> LDevID
esp_err_t enrollment_zero_touch(void);

// Fallback : HMAC proof -> operator confirm -> enroll
esp_err_t enrollment_fallback(void);

// Check cert expiry -> renewal J-30
esp_err_t enrollment_check_renewal(void);
