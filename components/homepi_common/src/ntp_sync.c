// components/homepi_common/src/ntp_sync.c
#include <inttypes.h>
#include "ntp_sync.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"

static const char *TAG = "ntp";
static EventGroupHandle_t ntp_events;
#define NTP_SYNCED_BIT BIT0

static void ntp_callback(struct timeval *tv) {
    xEventGroupSetBits(ntp_events, NTP_SYNCED_BIT);
}

esp_err_t ntp_sync_wait(uint32_t timeout_ms)
{
    ntp_events = xEventGroupCreate();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    // The Pi only: the IoT network has no route to the LAN or to an outside NTP server
    // (nftables), and keeping time local keeps the devices off the Internet. The former
    // fallback to the LAN router (192.168.0.1) could never be reached from here.
    esp_sntp_setservername(0, "192.168.200.1");
    sntp_set_time_sync_notification_cb(ntp_callback);
    esp_sntp_init();

    // Retry loop: 3 attempts of 10 s
    for (int attempt = 0; attempt < 3; attempt++) {
        EventBits_t bits = xEventGroupWaitBits(
            ntp_events, NTP_SYNCED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(10000)
        );
        if (bits & NTP_SYNCED_BIT) {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "NTP synced (attempt %d/3): %04d-%02d-%02d %02d:%02d:%02d UTC",
                     attempt + 1,
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "NTP attempt %d/3 timeout — retrying", attempt + 1);
        if (attempt < 2)
            esp_sntp_restart();
    }

    ESP_LOGE(TAG, "NTP failed after 3 attempts (30s total) — TLS impossible");
    esp_sntp_stop();
    vEventGroupDelete(ntp_events);
    return ESP_ERR_TIMEOUT;
}