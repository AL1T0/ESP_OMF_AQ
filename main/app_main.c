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

QueueHandle_t measurement_queue;
SemaphoreHandle_t mqtt_semaphore;

void app_main(void)
{
    //ESP_LOGI(MQTT_TAG, "[APP] Startup..");
    //ESP_LOGI(MQTT_TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    //ESP_LOGI(MQTT_TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("SENSORS_READ", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();

    // Sync time at boot (blocks up to ~30s max across retries)
    time_sync_init();
    //xTaskCreate(time_daily_resync_task, "TimeDailyResync", 3072, NULL, 2, NULL);

    // Initialize peripherals
    gpio_init();
    uart_init(UART_MHZin, RXDi_PIN, TXDi_PIN);
    i2c_master_init();
        
    // Queue for measurements
    measurement_queue = xQueueCreate(1, sizeof(measurement_q_t));
    xTaskCreate(sensors_reading_task, "sensorsReadingTask", 4096, NULL, configMAX_PRIORITIES, NULL);

    // Configure and initialize the http client
    http_client_app_start();
}
