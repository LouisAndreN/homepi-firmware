// main/data_buffer.c
#include "data_buffer.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "databuf";

// ── Layout partition (8KB = 0x2000) ──────────────────────────
// Offset 0x0000 : header (8 bytes)
//   [0..3] magic    0xD47A1001
//   [4..5] head     (prochain write, 0..MAX_ENTRIES-1)
//   [6..7] count    (nb entrées valides)
// Offset 0x0008 : entries (MAX_ENTRIES × ENTRY_SIZE bytes)

#define MAGIC           0xD47A1002   // était 0xD47A1001 — bump obligatoire :
                                      // buf_entry_t change de taille (40→60 octets),
                                      // sans ce bump l'ancien buffer flash serait
                                      // réinterprété avec le mauvais layout au boot
#define MAX_ENTRIES     200
#define ENTRY_SIZE      60    // ts(4) + T(4) + H(4) + CO2(4) + P(4) + lux(4)
                              //   + iaq(4) + pm1_0(4) + pm2_5(4) + pm4_0(4)
                              //   + pm10(4) + voc_raw(2) + nox_raw(2)
                              //   + rssi(1) + flags(1) + pad(6)
#define HEADER_OFFSET   0
#define ENTRIES_OFFSET  8
#define TOTAL_NEEDED    (ENTRIES_OFFSET + MAX_ENTRIES * ENTRY_SIZE) // 8008 bytes

typedef struct __attribute__((packed)) {
    uint32_t ts;
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
    uint16_t voc_raw;
    uint16_t nox_raw;
    int8_t   rssi;
    uint8_t  flags;      // bit0=scd41_ok bit1=bme688_ok bit2=bh1750_ok
                          // bit3=sht40_ok bit4=sps30_ok bit5=sgp41_ok
    uint8_t  _pad[6];
} buf_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t ts;
    float    temperature;
    float    humidity;
    float    co2;
    float    pressure;
    float    lux;
    float    iaq;
    int8_t   rssi;
    uint8_t  flags;      // bit0=scd41_ok, bit1=bme688_ok, bit2=bh1750_ok
    uint8_t  _pad[10];
} buf_entry_t;

_Static_assert(sizeof(buf_entry_t) == ENTRY_SIZE, "ENTRY_SIZE mismatch");

static const esp_partition_t *s_part    = NULL;
static SemaphoreHandle_t      s_mutex   = NULL;
static buf_header_t           s_hdr     = {0};

// ── Flash helpers ─────────────────────────────────────────────
// La partition flash NE PEUT PAS être écrite sans être effacée d'abord.
// On garde le header en RAM et on réécrit la partition au flush.
// Pour éviter les effacements inutiles, on utilise un cache RAM
// et on ne flush que sur les événements clés (reboot, MQTT reconnect).

static buf_entry_t s_cache[MAX_ENTRIES];
static bool        s_dirty = false;

static esp_err_t load_from_flash(void)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;

    buf_header_t hdr;
    esp_partition_read(s_part, HEADER_OFFSET, &hdr, sizeof(hdr));

    if (hdr.magic != MAGIC) {
        ESP_LOGW(TAG, "Partition vide ou corrompue — initialisation");
        memset(&s_hdr, 0, sizeof(s_hdr));
        s_hdr.magic = MAGIC;
        memset(s_cache, 0, sizeof(s_cache));
        return ESP_OK;
    }

    s_hdr = hdr;
    if (s_hdr.count > MAX_ENTRIES) s_hdr.count = MAX_ENTRIES;
    if (s_hdr.head  >= MAX_ENTRIES) s_hdr.head = 0;

    esp_partition_read(s_part, ENTRIES_OFFSET, s_cache, sizeof(s_cache));
    ESP_LOGI(TAG, "Loaded %d buffered readings from flash", s_hdr.count);
    return ESP_OK;
}

static esp_err_t save_to_flash(void)
{
    if (!s_part || !s_dirty) return ESP_OK;

    // Effacer la partition (obligatoire avant écriture flash)
    esp_err_t ret = esp_partition_erase_range(s_part, 0, s_part->size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Écrire header
    ret = esp_partition_write(s_part, HEADER_OFFSET, &s_hdr, sizeof(s_hdr));
    if (ret != ESP_OK) return ret;

    // Écrire entries
    ret = esp_partition_write(s_part, ENTRIES_OFFSET, s_cache, sizeof(s_cache));
    if (ret == ESP_OK) s_dirty = false;
    return ret;
}

// ── API publique ──────────────────────────────────────────────

esp_err_t data_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    s_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "datalog");
    if (!s_part) {
        ESP_LOGW(TAG, "Partition 'datalog' non trouvée — buffer RAM uniquement");
        // Fallback RAM : fonctionnel mais données perdues au reboot
        memset(&s_hdr, 0, sizeof(s_hdr));
        s_hdr.magic = MAGIC;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Partition datalog: offset=0x%08lx size=%lu bytes",
             (unsigned long)s_part->address, (unsigned long)s_part->size);

    load_from_flash();
    return ESP_OK;
}

esp_err_t data_buffer_push(const sensor_data_t *data)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    buf_entry_t e = {
        .ts          = (uint32_t)time(NULL),
        .temperature = data->temperature,
        .humidity    = data->humidity,
        .co2         = data->co2,
        .pressure    = data->pressure,
        .lux         = data->lux,
        .iaq         = data->iaq,
        .pm1_0       = data->pm1_0,
        .pm2_5       = data->pm2_5,
        .pm4_0       = data->pm4_0,
        .pm10        = data->pm10,
        .voc_raw     = data->voc_raw,
        .nox_raw     = data->nox_raw,
        .rssi        = data->rssi,
        .flags       = (data->scd41_ok  ? 0x01 : 0)
                    | (data->bme688_ok ? 0x02 : 0)
                    | (data->bh1750_ok ? 0x04 : 0)
                    | (data->sht40_ok  ? 0x08 : 0)
                    | (data->sps30_ok  ? 0x10 : 0)
                    | (data->sgp41_ok  ? 0x20 : 0),
    };

    s_cache[s_hdr.head] = e;
    s_hdr.head = (s_hdr.head + 1) % MAX_ENTRIES;
    if (s_hdr.count < MAX_ENTRIES) s_hdr.count++;
    s_dirty = true;

    // Sauvegarder en flash tous les 10 push
    // (compromis wear vs continuité)
    static int push_count = 0;
    if (++push_count >= 10) {
        push_count = 0;
        save_to_flash();
    }

    ESP_LOGD(TAG, "Buffered reading %d/%d (ts=%lu)",
             s_hdr.count, MAX_ENTRIES, (unsigned long)e.ts);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int data_buffer_flush(esp_mqtt_client_handle_t client,
                       const char *device_id, const char *topic)
{
    if (!s_mutex || s_hdr.count == 0) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int published = 0;
    int tail = ((int)s_hdr.head - (int)s_hdr.count
                + MAX_ENTRIES) % MAX_ENTRIES;

    for (int i = 0; i < (int)s_hdr.count; i++) {
        int idx = (tail + i) % MAX_ENTRIES;
        buf_entry_t *e = &s_cache[idx];

        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "ts",          (double)e->ts);
        cJSON_AddBoolToObject  (p, "buffered",    true);
        cJSON_AddNumberToObject(p, "temperature", e->temperature);
        cJSON_AddNumberToObject(p, "humidity",    e->humidity);
        if (e->flags & 0x01)
            cJSON_AddNumberToObject(p, "co2",     e->co2);
        if (e->flags & 0x02) {
            cJSON_AddNumberToObject(p, "pressure",e->pressure);
            cJSON_AddNumberToObject(p, "iaq",     e->iaq);
        }
        if (e->flags & 0x04)
            cJSON_AddNumberToObject(p, "lux",     e->lux);
        cJSON_AddNumberToObject(p, "rssi",        e->rssi);

        char *str = cJSON_PrintUnformatted(p);
        int msg_id = esp_mqtt_client_publish(client, topic, str, 0, 0, 0);
        free(str);
        cJSON_Delete(p);

        if (msg_id < 0) break;
        published++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_hdr.count -= published;
    if (s_hdr.count == 0) s_hdr.head = 0;
    s_dirty = true;
    save_to_flash();   // commit après flush

    xSemaphoreGive(s_mutex);
    if (published > 0)
        ESP_LOGI(TAG, "Flushed %d offline readings to MQTT", published);
    return published;
}

int data_buffer_count(void)
{
    return s_hdr.count;
}