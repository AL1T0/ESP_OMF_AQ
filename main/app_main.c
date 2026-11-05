/*
   ESP Air Quality IoT sensor

   Curated by Alena Grebneva for testing and validating OMF data ingress to CONNECT data services

   Based on ESP HTTP Client Example
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "headers.h"

static const char *TAG_APP = "APP";

QueueHandle_t measurement_queue;

// Logs heap health periodically and reboots if publishing has stalled.
// The heap numbers make leaks/fragmentation visible long before they bite:
// "free" should stay flat over hours; a steady decline means a leak.
static void health_task(void *pv)
{
    const TickType_t period = pdMS_TO_TICKS(60000);
    uint32_t ticks = 0;
    while (1) {
        vTaskDelay(period);

        if (++ticks % 10 == 0) { // every 10 min
            ESP_LOGI(TAG_APP, "Heap free=%u min=%u largest_block=%u, last OMF OK %u s ago",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)omf_seconds_since_last_success());
        }

        uint32_t stalled = omf_seconds_since_last_success();
        if (stalled > PUBLISH_STALL_RESTART_S) {
            ESP_LOGE(TAG_APP, "No successful publish for %u s, restarting", (unsigned)stalled);
            vTaskDelay(pdMS_TO_TICKS(500)); // let the log flush
            esp_restart();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG_APP, "Startup, IDF %s, reset reason %d, free heap %u",
             esp_get_idf_version(), (int)esp_reset_reason(), (unsigned)esp_get_free_heap_size());

    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_init_sta();

    // Sync time at boot (blocks up to ~30 s); SNTP keeps syncing in background
    time_sync_init();

    // Initialize peripherals
    gpio_init();
    uart_init(UART_MHZin, RXDi_PIN, TXDi_PIN);
    i2c_master_init();

    // Single-slot queue: the sensor task overwrites it, the publisher consumes it
    measurement_queue = xQueueCreate(1, sizeof(measurement_q_t));
    if (!measurement_queue) {
        ESP_LOGE(TAG_APP, "Failed to create measurement queue");
        esp_restart();
    }

    http_client_app_start();

    xTaskCreate(sensors_reading_task, "sensorsReading", 4096, NULL, PRIO_SENSORS, NULL);
    xTaskCreate(health_task, "health", 3072, NULL, PRIO_HEALTH, NULL);
}
