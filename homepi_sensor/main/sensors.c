// main/sensors.c
#include "nvs_manager.h"
#include "sensors.h"
#include "config.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_app_desc.h"
#include <stdio.h>


// Manage all configurations of known sensors
// New sensors will be added here


static const char *TAG = "sensors";


// ── Type des fonctions de driver ───────────────────────────────────────────
typedef esp_err_t (*sensor_init_fn)(void);
typedef esp_err_t (*sensor_read_fn)(sensor_data_t *out);

typedef struct {
    uint8_t         addr;
    const char     *name;           // nom canonique → construit hw_model
    const char     *capabilities;   // CSV des métriques publiées
    sensor_init_fn  init;
    sensor_read_fn  read;
} sensor_driver_t;


// ── Forward declarations — TOUTES les fonctions internes de mesure,
// requises car référencées dans SENSOR_DRIVERS[] plus bas mais définies
// plus loin dans le fichier ────────────────────────────────────────────────
static esp_err_t scd41_init(void);
static esp_err_t scd41_measure_once(float *co2, float *temp, float *hum);
static esp_err_t bme688_init(void);
static esp_err_t bme688_measure(float *temp, float *hum, float *press);
static esp_err_t bh1750_init(void);
static esp_err_t bh1750_read_lux(float *lux);
static esp_err_t sht40_init(void);
static esp_err_t sht40_measure(float *temp, float *hum);
static esp_err_t sps30_init(void);
static esp_err_t sps30_measure(float *pm1_0, float *pm2_5, float *pm4_0, float *pm10);
static esp_err_t sgp41_init(void);
static esp_err_t sgp41_measure_raw(float rh_pct, float temp_c, uint16_t *sraw_voc, uint16_t *sraw_nox);

// ── Read wrappers pour la dispatch table ────────────────────────────────────
// Chaque wrapper est responsable de poser SON PROPRE flag _ok dans out —
// c'est ce qui permet à sensors_read_all() de rester générique et de ne
// jamais avoir besoin d'être modifiée pour un nouveau capteur : ajouter un
// capteur = ajouter un wrapper + une ligne dans SENSOR_DRIVERS[], point final.

static esp_err_t bh1750_read(sensor_data_t *out)
{
    esp_err_t ret = bh1750_read_lux(&out->lux);
    out->bh1750_ok = (ret == ESP_OK);
    return ret;
}

static esp_err_t scd41_read(sensor_data_t *out)
{
    esp_err_t ret = scd41_measure_once(&out->co2, &out->temperature, &out->humidity);
    out->scd41_ok = (ret == ESP_OK);
    return ret;
}

static esp_err_t bme688_read(sensor_data_t *out)
{
    float t, h, p;
    esp_err_t ret = bme688_measure(&t, &h, &p);
    out->bme688_ok = (ret == ESP_OK);
    if (ret == ESP_OK) {
        out->pressure = p;
        out->iaq = 25.0f + fabsf(h - 45.0f) * 2.0f;
        // T/H seulement si aucun capteur T/H plus prioritaire n'a déjà écrit
        // (priorité = ordre de la dispatch table : SCD41 > BME688 > SHT40)
        if (out->temperature == 0.0f) {
            out->temperature = t;
            out->humidity    = h;
        }
    }
    return ret;
}

static esp_err_t sht40_read(sensor_data_t *out)
{
    float t, h;
    esp_err_t ret = sht40_measure(&t, &h);
    out->sht40_ok = (ret == ESP_OK);
    if (ret == ESP_OK && out->temperature == 0.0f) {
        out->temperature = t;
        out->humidity    = h;
    }
    return ret;
}

static esp_err_t sps30_read(sensor_data_t *out)
{
    float pm1, pm25, pm4, pm10;
    esp_err_t ret = sps30_measure(&pm1, &pm25, &pm4, &pm10);
    out->sps30_ok = (ret == ESP_OK);
    if (ret == ESP_OK) {
        out->pm1_0 = pm1;
        out->pm2_5 = pm25;
        out->pm4_0 = pm4;
        out->pm10  = pm10;
    }
    return ret;
}

static esp_err_t sgp41_read(sensor_data_t *out)
{
    // Compense avec la T/H déjà écrite par les drivers précédents dans out
    // (SGP41 DOIT rester en dernière position dans SENSOR_DRIVERS[] pour
    // que ceci soit garanti — voir commentaire sur la table plus bas).
    // Fallback 25°C/50%RH si aucun capteur T/H actif sur ce device.
    float rh = (out->humidity > 0.0f) ? out->humidity : 50.0f;
    float t  = (out->temperature != 0.0f) ? out->temperature : 25.0f;

    uint16_t voc_raw, nox_raw;
    esp_err_t ret = sgp41_measure_raw(rh, t, &voc_raw, &nox_raw);
    out->sgp41_ok = (ret == ESP_OK);
    if (ret == ESP_OK) {
        out->voc_raw = voc_raw;
        out->nox_raw = nox_raw;
    }
    return ret;
}

// ── Table de dispatch — ajouter un capteur = 1 wrapper + 1 ligne ici ───────
// ORDRE IMPORTANT : sensors_read_all() itère s_active[] dans cet ordre exact
// (préservé depuis la découverte). sgp41 doit rester en DERNIER car il
// consomme out->temperature/humidity déjà écrits par les drivers précédents.
static const sensor_driver_t SENSOR_DRIVERS[] = {
    { 0x23, "bh1750",  "lux",                          bh1750_init,  bh1750_read  },
    { 0x62, "scd41",   "co2,temperature,humidity",      scd41_init,   scd41_read   },
    { 0x76, "bme688",  "iaq,pressure,temperature,humidity", bme688_init, bme688_read },
    { 0x44, "sht40",   "temperature,humidity",          sht40_init,   sht40_read   },
    { 0x69, "sps30",   "pm1_0,pm2_5,pm4_0,pm10",         sps30_init,   sps30_read   },
    { 0x59, "sgp41",   "voc_raw,nox_raw",                sgp41_init,   sgp41_read   },  // DOIT rester en dernier
    // { 0x40, "ina219",  "current",                    ina219_init,  ina219_read  },
    // { 0x68, "mpu6050", "acceleration",               mpu6050_init, mpu6050_read },
    // { 0x29, "vl53l0x", "distance",                   vl53l0x_init, vl53l0x_read },
};
#define N_DRIVERS (sizeof(SENSOR_DRIVERS) / sizeof(SENSOR_DRIVERS[0]))

// ── État runtime ───────────────────────────────────────────────────────────
static const sensor_driver_t *s_active[8];
static int                    s_active_count = 0;
static char                   s_capabilities_csv[256];  // "co2,temperature,lux"
static char                   s_hw_model[128];          // "bh1750+scd41"

// ── API publique ───────────────────────────────────────────────────────────
const char *sensors_get_capabilities_csv(void) { return s_capabilities_csv; }
const char *sensors_get_hw_model(void)          { return s_hw_model; }

esp_err_t sensors_discover_and_init(void)
{
    s_active_count = 0;
    s_capabilities_csv[0] = '\0';
    s_hw_model[0] = '\0';

    for (int i = 0; i < (int)N_DRIVERS; i++) {
        // Sonde I2C : le master envoie l'adresse, attend ACK
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd,
            (SENSOR_DRIVERS[i].addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);

        if (r != ESP_OK) continue;  // pas de réponse → absent

        // Présent → init
        esp_err_t init_ret = SENSOR_DRIVERS[i].init();
        if (init_ret != ESP_OK) {
            ESP_LOGW(TAG, "✗ %s @ 0x%02X : présent mais init échoué (%s)",
                     SENSOR_DRIVERS[i].name,
                     SENSOR_DRIVERS[i].addr,
                     esp_err_to_name(init_ret));
            continue;
        }

        ESP_LOGI(TAG, "✓ %s @ 0x%02X", SENSOR_DRIVERS[i].name, SENSOR_DRIVERS[i].addr);
        s_active[s_active_count++] = &SENSOR_DRIVERS[i];

        // Construire capabilities CSV (dédupliquer)
        // Simple : on concatène, le Pi déduplique
        if (s_capabilities_csv[0]) strncat(s_capabilities_csv, ",", sizeof(s_capabilities_csv)-1);
        strncat(s_capabilities_csv, SENSOR_DRIVERS[i].capabilities, sizeof(s_capabilities_csv)-1);

        // Construire hw_model : "bh1750+scd41" (ordre de scan = ordre adresses)
        if (s_hw_model[0]) strncat(s_hw_model, "+", sizeof(s_hw_model)-1);
        strncat(s_hw_model, SENSOR_DRIVERS[i].name, sizeof(s_hw_model)-1);

        if (s_active_count >= 8) break;
    }

    // Persister en NVS pour éviter re-scan si boot rapide
    nvs_mgr_set_string("hw_model", s_hw_model);
    nvs_mgr_set_string("capabilities", s_capabilities_csv);

    ESP_LOGI(TAG, "hw_model: %s", s_hw_model);
    ESP_LOGI(TAG, "capabilities: %s", s_capabilities_csv);

    return s_active_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// Point d'entrée UNIQUE pour lire tous les capteurs actifs — générique,
// jamais besoin d'être modifiée pour un nouveau capteur. Chaque driver
// pose lui-même son flag _ok dans out (voir wrappers plus haut).
esp_err_t sensors_read_all(sensor_data_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < s_active_count; i++) {
        s_active[i]->read(out);  // erreurs individuelles déjà loguées par chaque driver si besoin ; on continue malgré un échec ponctuel
    }
    return ESP_OK;
}



// ── I2C helpers ───────────────────────────────────────────────

static esp_err_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    return i2c_master_write_to_device(I2C_NUM_0, addr, data, len,
                                      pdMS_TO_TICKS(100));
}

static esp_err_t i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    return i2c_master_read_from_device(I2C_NUM_0, addr, data, len,
                                       pdMS_TO_TICKS(100));
}

static esp_err_t i2c_write_read(uint8_t addr,
                                  const uint8_t *cmd, size_t cmd_len,
                                  uint8_t *data, size_t data_len)
{
    return i2c_master_write_read_device(I2C_NUM_0, addr,
                                        cmd, cmd_len,
                                        data, data_len,
                                        pdMS_TO_TICKS(100));
}

// ── CRC8 partagé (poly 0x31, init 0xFF) — SCD41, SHT40, SPS30, SGP41 ─────
// Tous les capteurs Sensirion de ce firmware utilisent ce même CRC8.

static uint8_t scd41_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

// ── SCD41 ─────────────────────────────────────────────────────

static esp_err_t scd41_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_write(I2C_SCD41_ADDR, buf, 2);
}

static esp_err_t scd41_init(void)
{
    // stop_periodic_measurement au cas où déjà actif
    scd41_send_cmd(0x3F86);
    vTaskDelay(pdMS_TO_TICKS(500));

    // reinit
    scd41_send_cmd(0x3646);
    vTaskDelay(pdMS_TO_TICKS(30));

    // Vérifier présence : get_serial_number (0x3682)
    uint8_t cmd[] = {0x36, 0x82};
    uint8_t resp[9];
    esp_err_t ret = i2c_write_read(I2C_SCD41_ADDR, cmd, 2, resp, 9);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 not found (I2C addr 0x%02X)", I2C_SCD41_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t serial = (resp[0] << 16) | (resp[1] << 8) | resp[3];
    ESP_LOGI(TAG, "SCD41 found, serial: 0x%06X", (unsigned)serial);
    return ESP_OK;
}

static esp_err_t scd41_measure_once(float *co2, float *temp, float *hum)
{
    // measure_single_shot (0x219D) — single-shot, low power
    esp_err_t ret = scd41_send_cmd(0x219D);
    if (ret != ESP_OK) return ret;

    // Attendre fin de mesure (5s pour single-shot)
    vTaskDelay(pdMS_TO_TICKS(5100));

    // read_measurement (0xEC05)
    uint8_t cmd[] = {0xEC, 0x05};
    uint8_t data[9];
    ret = i2c_write_read(I2C_SCD41_ADDR, cmd, 2, data, 9);
    if (ret != ESP_OK) return ret;

    // Vérifier CRC des 3 mots
    if (scd41_crc8(&data[0], 2) != data[2] ||
        scd41_crc8(&data[3], 2) != data[5] ||
        scd41_crc8(&data[6], 2) != data[8]) {
        ESP_LOGE(TAG, "SCD41 CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t co2_raw  = (data[0] << 8) | data[1];
    uint16_t temp_raw = (data[3] << 8) | data[4];
    uint16_t hum_raw  = (data[6] << 8) | data[7];

    *co2  = (float)co2_raw;
    *temp = -45.0f + 175.0f * temp_raw / 65535.0f;
    *hum  = 100.0f * hum_raw / 65535.0f;

    // Clamp valeurs plausibles
    if (*co2 < 400 || *co2 > 5000)  return ESP_ERR_INVALID_RESPONSE;
    if (*temp < -20 || *temp > 85)   return ESP_ERR_INVALID_RESPONSE;
    if (*hum < 0 || *hum > 100)      return ESP_ERR_INVALID_RESPONSE;

    return ESP_OK;
}


// ── SHT40 ─────────────────────────────────────────────────────

static bool s_sht40_ready = false;

static esp_err_t sht40_init(void)
{
    // Soft reset (0x94)
    uint8_t cmd = 0x94;
    esp_err_t ret = i2c_write(I2C_SHT40_ADDR, &cmd, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 not found (I2C addr 0x%02X)", I2C_SHT40_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "SHT40 found");
    s_sht40_ready = true;
    return ESP_OK;
}

static esp_err_t sht40_measure(float *temp, float *hum)
{
    if (!s_sht40_ready) return ESP_ERR_INVALID_STATE;

    // Measure T/RH high precision (0xFD)
    uint8_t cmd = 0xFD;
    esp_err_t ret = i2c_write(I2C_SHT40_ADDR, &cmd, 1);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10)); // ~8.3ms max en high-precision

    uint8_t data[6];
    ret = i2c_read(I2C_SHT40_ADDR, data, 6);
    if (ret != ESP_OK) return ret;

    // CRC8 même poly que SCD41 — réutilise scd41_crc8()
    if (scd41_crc8(&data[0], 2) != data[2] ||
        scd41_crc8(&data[3], 2) != data[5]) {
        ESP_LOGE(TAG, "SHT40 CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t t_raw = (data[0] << 8) | data[1];
    uint16_t h_raw = (data[3] << 8) | data[4];

    *temp = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    *hum  = -6.0f  + 125.0f * (float)h_raw / 65535.0f;
    if (*hum > 100.0f) *hum = 100.0f;
    if (*hum < 0.0f)   *hum = 0.0f;

    return ESP_OK;
}


// ── BME688 ────────────────────────────────────────────────────
// Température, humidité, pression — sans BSEC (IAQ approximatif)
// Le vrai IAQ nécessite la bibliothèque Bosch BSEC (propriétaire)

typedef struct {
    // Calibration température
    uint16_t par_t1;
    int16_t  par_t2;
    int8_t   par_t3;
    // Calibration pression
    uint16_t par_p1;
    int16_t  par_p2;
    int8_t   par_p3;
    int16_t  par_p4;
    int16_t  par_p5;
    int8_t   par_p6;
    int8_t   par_p7;
    int16_t  par_p8;
    int16_t  par_p9;
    uint8_t  par_p10;
    // Calibration humidité
    uint16_t par_h1;
    uint16_t par_h2;
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;
    // t_fine (calculé lors de la mesure T)
    int32_t  t_fine;
} bme688_calib_t;

static bme688_calib_t s_bme688_calib = {0};
static bool s_bme688_ready = false;

static esp_err_t bme688_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_write_read(I2C_BME688_ADDR, &reg, 1, val, 1);
}

static esp_err_t bme688_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_write(I2C_BME688_ADDR, buf, 2);
}

static esp_err_t bme688_read_calib(void)
{
    uint8_t coeff1[26], coeff2[16];

    // Bloc 1 : 0x89 → 0xA1 (25 bytes)
    for (int i = 0; i < 26; i++) {
        uint8_t reg = 0x89 + i;
        bme688_read_reg(reg, &coeff1[i]);
    }
    // Bloc 2 : 0xE1 → 0xF0 (16 bytes)
    for (int i = 0; i < 16; i++) {
        uint8_t reg = 0xE1 + i;
        bme688_read_reg(reg, &coeff2[i]);
    }

    s_bme688_calib.par_t1 = (uint16_t)(coeff2[9]  << 8 | coeff2[8]);
    // FIX : par_t2/par_t3 étaient décalés d'un octet vers le bas
    // (registres réels 0x8A/0x8B pour t2, 0x8C pour t3 → offsets 1-2 et 3
    // depuis 0x89, PAS 0-1 et 2). Ce décalage corrompait t_fine, qui est
    // aussi réutilisé tel quel par bme688_comp_press() — d'où température
    // ET pression fausses simultanément (-4°C constant, 869 hPa au lieu
    // de ~940-960 hPa attendus à l'altitude de Nagano).
    s_bme688_calib.par_t2 = (int16_t) (coeff1[2]  << 8 | coeff1[1]);
    s_bme688_calib.par_t3 = (int8_t)   coeff1[3];

    s_bme688_calib.par_p1  = (uint16_t)(coeff1[6]  << 8 | coeff1[5]);
    s_bme688_calib.par_p2  = (int16_t) (coeff1[8]  << 8 | coeff1[7]);
    s_bme688_calib.par_p3  = (int8_t)   coeff1[9];
    s_bme688_calib.par_p4  = (int16_t) (coeff1[12] << 8 | coeff1[11]);
    s_bme688_calib.par_p5  = (int16_t) (coeff1[14] << 8 | coeff1[13]);
    s_bme688_calib.par_p6  = (int8_t)   coeff1[16];
    s_bme688_calib.par_p7  = (int8_t)   coeff1[15];
    s_bme688_calib.par_p8  = (int16_t) (coeff1[20] << 8 | coeff1[19]);
    s_bme688_calib.par_p9  = (int16_t) (coeff1[22] << 8 | coeff1[21]);
    s_bme688_calib.par_p10 = coeff1[23];

    s_bme688_calib.par_h1  = (uint16_t)((coeff2[2] << 4) | (coeff2[1] & 0x0F));
    s_bme688_calib.par_h2  = (uint16_t)((coeff2[0] << 4) | (coeff2[1] >> 4));
    s_bme688_calib.par_h3  = (int8_t)   coeff2[3];
    s_bme688_calib.par_h4  = (int8_t)   coeff2[4];
    s_bme688_calib.par_h5  = (int8_t)   coeff2[5];
    s_bme688_calib.par_h6  =            coeff2[6];
    s_bme688_calib.par_h7  = (int8_t)   coeff2[7];

    return ESP_OK;
}

// Compensation température (retourne °C, met à jour t_fine)
static float bme688_comp_temp(uint32_t adc_T)
{
    bme688_calib_t *c = &s_bme688_calib;
    float var1 = ((float)adc_T / 16384.0f - (float)c->par_t1 / 1024.0f)
                  * (float)c->par_t2;
    float var2 = (((float)adc_T / 131072.0f - (float)c->par_t1 / 8192.0f)
                   * ((float)adc_T / 131072.0f - (float)c->par_t1 / 8192.0f))
                  * ((float)c->par_t3 * 16.0f);
    c->t_fine = (int32_t)(var1 + var2);
    return (var1 + var2) / 5120.0f;
}

// Compensation pression (hPa)
static float bme688_comp_press(uint32_t adc_P)
{
    bme688_calib_t *c = &s_bme688_calib;
    float var1 = (float)c->t_fine / 2.0f - 64000.0f;
    float var2 = var1 * var1 * (float)c->par_p6 / 131072.0f;
    var2 = var2 + var1 * (float)c->par_p5 * 2.0f;
    var2 = var2 / 4.0f + (float)c->par_p4 * 65536.0f;
    var1 = ((float)c->par_p3 * var1 * var1 / 524288.0f
            + (float)c->par_p2 * var1) / 524288.0f;
    var1 = (1.0f + var1 / 32768.0f) * (float)c->par_p1;
    if (var1 == 0.0f) return 0.0f;
    float press = 1048576.0f - (float)adc_P;
    press = ((press - var2 / 4096.0f) * 6250.0f) / var1;
    var1 = (float)c->par_p9 * press * press / 2147483648.0f;
    var2 = press * ((float)c->par_p8 / 32768.0f);
    press += (var1 + var2 + (float)c->par_p7) / 16.0f;
    return press / 100.0f; // Pa → hPa
}

// Compensation humidité (%)
static float bme688_comp_hum(uint16_t adc_H)
{
    bme688_calib_t *c = &s_bme688_calib;
    float temp_comp = (float)c->t_fine / 5120.0f;
    float var1 = (float)adc_H - ((float)c->par_h1 * 16.0f)
                 - ((temp_comp * (float)c->par_h3 / 200.0f) * 16.0f);
    float var2 = var1 * ((float)c->par_h2 / 262144.0f
                  * (1.0f + (float)c->par_h4 / 16384.0f * temp_comp
                     + (float)c->par_h5 / 1048576.0f * temp_comp * temp_comp));
    float var3 = (float)c->par_h6 / 16384.0f;
    float var4 = (float)c->par_h7 / 2097152.0f;
    float hum  = var2 + (var3 + var4 * var2) * var2 * var2;
    if (hum > 100.0f) hum = 100.0f;
    if (hum < 0.0f)   hum = 0.0f;
    return hum;
}

static esp_err_t bme688_init(void)
{
    // Vérifier chip_id (0xD0 register → 0x61 pour BME688)
    uint8_t chip_id;
    esp_err_t ret = bme688_read_reg(0xD0, &chip_id);
    if (ret != ESP_OK || chip_id != 0x61) {
        ESP_LOGW(TAG, "BME688 not found (chip_id=0x%02X, expected 0x61)", chip_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "BME688 found (chip_id=0x%02X)", chip_id);

    // Soft reset
    bme688_write_reg(0xE0, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Lire calibration
    bme688_read_calib();

    // Mode forced, OSR temp×2, OSR press×4, OSR hum×1
    bme688_write_reg(0x74, 0x54); // ctrl_meas : osrs_t=010, osrs_p=100
    bme688_write_reg(0x72, 0x01); // ctrl_hum  : osrs_h=001
    bme688_write_reg(0x75, 0x08); // config    : filter coeff=2

    s_bme688_ready = true;
    return ESP_OK;
}

static esp_err_t bme688_measure(float *temp, float *hum, float *press)
{
    if (!s_bme688_ready) return ESP_ERR_INVALID_STATE;

    // Déclencher forced mode
    uint8_t ctrl;
    bme688_read_reg(0x74, &ctrl);
    bme688_write_reg(0x74, (ctrl & 0xFC) | 0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Attendre fin de mesure (meas_status_0 bit 5)
    int timeout = 20;
    uint8_t status;
    do {
        vTaskDelay(pdMS_TO_TICKS(10));
        bme688_read_reg(0x1D, &status);
    } while ((status & 0x20) && --timeout > 0);

    // Lire données brutes (0x1F → 0x26)
    uint8_t raw[8];
    for (int i = 0; i < 8; i++) {
        uint8_t reg = 0x1F + i;
        bme688_read_reg(reg, &raw[i]);
    }

    uint32_t adc_T = ((uint32_t)raw[3] << 12) | ((uint32_t)raw[4] << 4)
                     | (raw[5] >> 4);
    uint32_t adc_P = ((uint32_t)raw[0] << 12) | ((uint32_t)raw[1] << 4)
                     | (raw[2] >> 4);
    uint16_t adc_H = ((uint16_t)raw[6] << 8) | raw[7];

    *temp  = bme688_comp_temp(adc_T);
    *press = bme688_comp_press(adc_P);
    *hum   = bme688_comp_hum(adc_H);

    return ESP_OK;
}


// ── SPS30 ─────────────────────────────────────────────────────
// Note: SPS30 utilise des commandes 16-bit (pas 8-bit comme les autres),
// et le même CRC8 poly 0x31/init 0xFF que les autres capteurs Sensirion.

static bool s_sps30_ready = false;

static esp_err_t sps30_send_cmd16(uint16_t cmd)
{
    uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_write(I2C_SPS30_ADDR, buf, 2);
}

static esp_err_t sps30_init(void)
{
    // Wake-up (au cas où en veille) — ignore l'erreur, pas toujours nécessaire
    sps30_send_cmd16(0x1103);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Start measurement — 0x0010, arg 0x0300 (float output), CRC de l'arg
    uint8_t buf[5] = { 0x00, 0x10, 0x03, 0x00, 0x00 };
    buf[4] = scd41_crc8(&buf[2], 2);
    esp_err_t ret = i2c_write(I2C_SPS30_ADDR, buf, 5);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPS30 not found (I2C addr 0x%02X)", I2C_SPS30_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "SPS30 found, measurement started");
    s_sps30_ready = true;
    return ESP_OK;
}

static float sps30_words_to_float(const uint8_t *d)
{
    // Big-endian IEEE754, 2 mots de 16 bits chacun avec CRC individuel déjà validé en amont
    uint32_t bits = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
                   | ((uint32_t)d[3] << 8)  | (uint32_t)d[4];
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static esp_err_t sps30_measure(float *pm1_0, float *pm2_5, float *pm4_0, float *pm10)
{
    if (!s_sps30_ready) return ESP_ERR_INVALID_STATE;

    // Read data-ready flag (0x0202) avant de lire — évite de lire une trame périmée
    uint8_t rcmd[2] = {0x02, 0x02};
    uint8_t rdy[3];
    esp_err_t ret = i2c_write_read(I2C_SPS30_ADDR, rcmd, 2, rdy, 3);
    if (ret != ESP_OK) return ret;
    if (scd41_crc8(&rdy[0], 2) != rdy[2]) return ESP_ERR_INVALID_CRC;
    if (rdy[1] == 0) return ESP_ERR_NOT_FINISHED; // pas encore de nouvelle donnée

    // Read measured values (0x0300) — 60 bytes = 10 floats × (4 data + 2 CRC)
    uint8_t cmd[2] = {0x03, 0x00};
    uint8_t data[60];
    ret = i2c_write_read(I2C_SPS30_ADDR, cmd, 2, data, 60);
    if (ret != ESP_OK) return ret;

    // Vérifier les 10 CRC (un tous les 3 octets : 2 data + 1 crc, ×2 mots par float)
    for (int i = 0; i < 60; i += 3) {
        if (scd41_crc8(&data[i], 2) != data[i+2]) {
            ESP_LOGE(TAG, "SPS30 CRC error at offset %d", i);
            return ESP_ERR_INVALID_CRC;
        }
    }

    // Ordre des floats : PM1.0, PM2.5, PM4.0, PM10, puis nombre de particules (ignoré ici), puis typical size
    *pm1_0 = sps30_words_to_float(&data[0]);
    *pm2_5 = sps30_words_to_float(&data[6]);
    *pm4_0 = sps30_words_to_float(&data[12]);
    *pm10  = sps30_words_to_float(&data[18]);

    return ESP_OK;
}


// ── SGP41 ─────────────────────────────────────────────────────
// ATTENTION : ce driver lit les signaux BRUTS (SRAW_VOC / SRAW_NOX).
// Ce ne sont PAS des index VOC/NOx utilisables tels quels — Sensirion
// exige de les faire passer par son "Gas Index Algorithm" (bibliothèque
// C dédiée, stateful, avec calibration adaptative dans le temps) pour
// produire un index 0-500 comparable dans le temps. Sans cet algorithme,
// publier ces valeurs comme "VOC index" serait trompeur — elles dérivent
// avec la température/humidité ambiante et n'ont aucune échelle absolue
// stable. Voir https://github.com/Sensirion/gas-index-algorithm
// Décision architecturale : appliquer cet algorithme côté Pi (Python),
// cohérent avec le principe déjà établi pour la calibration NILM
// (calcul server-side, jamais en dur dans le firmware).

static bool s_sgp41_ready = false;
static bool s_sgp41_conditioned = false;
static int64_t s_sgp41_init_time_us = 0;

static esp_err_t sgp41_init(void)
{
    // Get serial number (0x3682) pour confirmer présence
    uint8_t cmd[2] = {0x36, 0x82};
    uint8_t resp[9];
    esp_err_t ret = i2c_write_read(I2C_SGP41_ADDR, cmd, 2, resp, 9);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP41 not found (I2C addr 0x%02X)", I2C_SGP41_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SGP41 found");
    s_sgp41_init_time_us = esp_timer_get_time();
    s_sgp41_conditioned = false;
    s_sgp41_ready = true;
    return ESP_OK;
}

// Compensated raw signals — requiert T/H ambiante en entrée
static esp_err_t sgp41_measure_raw(float rh_pct, float temp_c, uint16_t *sraw_voc, uint16_t *sraw_nox)
{
    if (!s_sgp41_ready) return ESP_ERR_INVALID_STATE;

    uint16_t rh_ticks   = (uint16_t)((rh_pct / 100.0f) * 65535.0f);
    uint16_t temp_ticks = (uint16_t)(((temp_c + 45.0f) / 175.0f) * 65535.0f);

    // Phase de conditionnement obligatoire : 10s après power-on, SGP41 doit
    // tourner en mode "conditioning" (0x2612) avant la première mesure combinée
    // (0x2619). Sauter cette étape donne des SRAW_VOC non fiables au démarrage.
    if (!s_sgp41_conditioned) {
        int64_t elapsed_us = esp_timer_get_time() - s_sgp41_init_time_us;
        if (elapsed_us < 10000000) {
            uint8_t cbuf[5] = {0x26, 0x12, (uint8_t)(rh_ticks>>8), (uint8_t)rh_ticks, 0};
            cbuf[4] = scd41_crc8(&cbuf[2], 2);
            i2c_write(I2C_SGP41_ADDR, cbuf, 5);
            vTaskDelay(pdMS_TO_TICKS(50));
            return ESP_ERR_NOT_FINISHED; // encore en conditionnement, pas de mesure valide
        }
        s_sgp41_conditioned = true;
        ESP_LOGI(TAG, "SGP41 conditioning done — starting compensated measurements");
    }

    // Measure raw signals compensés (0x2619)
    uint8_t buf[8];
    buf[0] = 0x26; buf[1] = 0x19;
    buf[2] = (uint8_t)(rh_ticks >> 8);   buf[3] = (uint8_t)rh_ticks;
    buf[4] = scd41_crc8(&buf[2], 2);
    buf[5] = (uint8_t)(temp_ticks >> 8); buf[6] = (uint8_t)temp_ticks;
    buf[7] = scd41_crc8(&buf[5], 2);

    esp_err_t ret = i2c_write(I2C_SGP41_ADDR, buf, 8);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[6];
    ret = i2c_read(I2C_SGP41_ADDR, data, 6);
    if (ret != ESP_OK) return ret;

    if (scd41_crc8(&data[0], 2) != data[2] ||
        scd41_crc8(&data[3], 2) != data[5]) {
        ESP_LOGE(TAG, "SGP41 CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    *sraw_voc = (data[0] << 8) | data[1];
    *sraw_nox = (data[3] << 8) | data[4];
    return ESP_OK;
}


// ── BH1750 ────────────────────────────────────────────────────

static bool s_bh1750_ready = false;

static esp_err_t bh1750_init(void)
{
    // Power on (0x01)
    uint8_t cmd = 0x01;
    esp_err_t ret = i2c_write(I2C_BH1750_ADDR, &cmd, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 not found (I2C addr 0x%02X)", I2C_BH1750_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    // Reset data register
    cmd = 0x07;
    i2c_write(I2C_BH1750_ADDR, &cmd, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "BH1750 found");
    s_bh1750_ready = true;
    return ESP_OK;
}

static esp_err_t bh1750_read_lux(float *lux)
{
    if (!s_bh1750_ready) return ESP_ERR_INVALID_STATE;

    // One-time high-resolution mode (0x20)
    uint8_t cmd = 0x20;
    i2c_write(I2C_BH1750_ADDR, &cmd, 1);
    vTaskDelay(pdMS_TO_TICKS(180)); // ~120ms + marge

    uint8_t data[2];
    esp_err_t ret = i2c_read(I2C_BH1750_ADDR, data, 2);
    if (ret != ESP_OK) return ret;

    *lux = (float)((data[0] << 8) | data[1]) / 1.2f;
    return ESP_OK;
}

// ── API publique ──────────────────────────────────────────────

esp_err_t sensors_init(void)
{
    // I2C master init
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA,
        .scl_io_num       = I2C_MASTER_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(I2C_NUM_0, &conf);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    // Init capteurs (non-fatal si absent)
    return sensors_discover_and_init();
}

esp_err_t sensors_publish(esp_mqtt_client_handle_t client,
                           const char *device_id,
                           const char *topic)
{
    sensor_data_t data;
    esp_err_t ret = sensors_read_all(&data);
    if (ret != ESP_OK) return ret;

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "ts",  (double)time(NULL));

    if (data.scd41_ok || data.bme688_ok || data.sht40_ok) {
        cJSON_AddNumberToObject(payload, "temperature",
                                roundf(data.temperature * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(payload, "humidity",
                                roundf(data.humidity * 10.0f) / 10.0f);
    }
    if (data.scd41_ok)
        cJSON_AddNumberToObject(payload, "co2", (int)data.co2);
    if (data.bme688_ok) {
        cJSON_AddNumberToObject(payload, "pressure",
                                roundf(data.pressure * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(payload, "iaq",
                                roundf(data.iaq));
    }
    if (data.bh1750_ok)
        cJSON_AddNumberToObject(payload, "lux", (int)data.lux);

    if (data.sps30_ok) {
        cJSON_AddNumberToObject(payload, "pm1_0", roundf(data.pm1_0 * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(payload, "pm2_5", roundf(data.pm2_5 * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(payload, "pm4_0", roundf(data.pm4_0 * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(payload, "pm10",  roundf(data.pm10  * 10.0f) / 10.0f);
    }

    if (data.sgp41_ok) {
        // Signaux BRUTS non calibrés — nommage explicite volontaire.
        // Ne PAS renommer en "voc_index"/"nox_index" tant que le Gas Index
        // Algorithm Sensirion n'est pas appliqué côté Pi.
        cJSON_AddNumberToObject(payload, "voc_raw", data.voc_raw);
        cJSON_AddNumberToObject(payload, "nox_raw", data.nox_raw);
    }

    cJSON_AddNumberToObject(payload, "rssi", data.rssi);
    cJSON_AddStringToObject(payload, "fw", esp_app_get_description()->version);

    // ── hw_model ──────────────────────────────────────────────────
    char hw_model[32] = {0};
    nvs_mgr_get_string("hw_model", hw_model, sizeof(hw_model));
    cJSON_AddStringToObject(payload, "hw_model",
        hw_model[0] ? hw_model : "esp32c3");

    // ── active_sensors (filtrable dans InfluxDB) ──────────────────
    char active_sensors[96] = {0};
    if (data.scd41_ok)  strncat(active_sensors, "scd41,",  sizeof(active_sensors)-1);
    if (data.bme688_ok) strncat(active_sensors, "bme688,", sizeof(active_sensors)-1);
    if (data.bh1750_ok) strncat(active_sensors, "bh1750,", sizeof(active_sensors)-1);
    if (data.sht40_ok)  strncat(active_sensors, "sht40,",  sizeof(active_sensors)-1);
    if (data.sps30_ok)  strncat(active_sensors, "sps30,",  sizeof(active_sensors)-1);
    if (data.sgp41_ok)  strncat(active_sensors, "sgp41,",  sizeof(active_sensors)-1);
    // Supprimer la virgule finale
    size_t slen = strlen(active_sensors);
    if (slen > 0 && active_sensors[slen-1] == ',')
        active_sensors[slen-1] = '\0';
    cJSON_AddStringToObject(payload, "active_sensors", active_sensors);

    char *str = cJSON_PrintUnformatted(payload);
    int msg_id = esp_mqtt_client_publish(client, topic, str, 0, 1, 0);
    ESP_LOGI(TAG, "Published (msg_id=%d): %s", msg_id, str);

    free(str);
    cJSON_Delete(payload);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}