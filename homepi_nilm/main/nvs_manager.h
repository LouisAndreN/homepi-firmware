// main/nvs_manager.h
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t nvs_mgr_init(void);

// Config partition (nvs)
esp_err_t nvs_mgr_get_string(const char *key, char *buf, size_t len);
esp_err_t nvs_mgr_set_string(const char *key, const char *val);
esp_err_t nvs_mgr_get_u8(const char *key, uint8_t *val);
esp_err_t nvs_mgr_set_u8(const char *key, uint8_t val);

// Certs partition (nvs_certs)
esp_err_t nvs_mgr_get_cert(const char *key, char *buf, size_t len);
esp_err_t nvs_mgr_set_cert(const char *key, const char *pem);
esp_err_t nvs_mgr_get_binary(const char *key, uint8_t *buf, size_t *len);

// LDevID A/B swap atomique
esp_err_t nvs_mgr_store_ldevid(const char *cert_pem, const char *key_pem);
esp_err_t nvs_mgr_get_active_ldevid(char *cert_buf, size_t cert_len, char *key_buf, size_t key_len);
