// time_sync.c
// SNTP time synchronization for ESP-IDF – boot sync + daily resync
// Based on the official LwIP SNTP example, adapted to your project layout.

#include "headers.h"

static const char *TAG_TS = "TIME_SYNC";

// Optional: notification when time is synchronized
static void time_sync_notification_cb(struct timeval *tv)
{
    time_t now = 0; char iso[32];
    time(&now);
    strftime(iso, sizeof(iso), "%FT%TZ", gmtime(&now));
    ESP_LOGI(TAG_TS, "Time synchronized: %s", iso);
}

/**
 * time_sync_init()
 * - Assumes Wi-Fi is already connected.
 * - Initializes SNTP with multiple servers and blocks (with timeout) until the first sync.
 * - Sets timezone to ART (Argentina, UTC-3) – adjust if you prefer a TZ database name.
 */
void time_sync_init(void)
{
    // Timezone (pick one)
    setenv("TZ", "ART-3", 1);
    // If zoneinfo is enabled in your build, prefer the canonical name:
    // setenv("TZ", "America/Argentina/Buenos_Aires", 1);
    tzset();

    #if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        esp_sntp_config_t cfg =
            ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
                ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org", "time.cloudflare.com"));
    #else
        esp_sntp_config_t cfg =
            ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    #endif
    cfg.sync_cb = time_sync_notification_cb;
    cfg.smooth_sync = false;

    ESP_LOGI(TAG_TS, "Initializing SNTP");
    esp_netif_sntp_init(&cfg);

    // Wait for time to be set (retry up to ~30s total)
    const int retry_max = 15;
    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) == ESP_ERR_TIMEOUT && ++retry < retry_max) {
        ESP_LOGI(TAG_TS, "Waiting for system time to be set... (%d/%d)", retry, retry_max);
    }

    time_t now = 0; char iso[32];
    time(&now);
    if (now > 1700000000) { // sanity check (~2023-11-14)
        strftime(iso, sizeof(iso), "%FT%TZ", gmtime(&now));
        ESP_LOGI(TAG_TS, "Initial SNTP sync OK: %s", iso);
    } else {
        ESP_LOGW(TAG_TS, "Initial SNTP sync timed out; continuing with unsynchronized clock");
    }
}

/**
 * time_daily_resync_task()
 * - Triggers an SNTP restart once per day.
 * - Light wait loop to confirm the sync completes.
 */
/*void time_daily_resync_task(void *pvParameters)
{
    const TickType_t one_day = pdMS_TO_TICKS(24 * 60 * 60 * 1000);
    while(1) {
        vTaskDelay(one_day);
        ESP_LOGI(TAG_TS, "Daily SNTP resync");
        // Restart underlying SNTP client (safe to call while initialized)
        sntp_restart();

        // Optionally wait briefly until status reports COMPLETED
        int tries = 0;
        while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && tries++ < 20) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
*/