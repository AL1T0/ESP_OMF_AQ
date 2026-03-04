// wifi_handler.c
#include "headers.h"
#include "keys.h"

EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "WI-FI station";
static int s_retry_num = 0;

static TaskHandle_t s_wifi_reconnect_task_handle = NULL;

/**
 * @brief Wi-Fi event handler.
 *
 * Only signals the reconnect task, does not reconnect inline.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Always clear both bits before attempting connect
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        xTaskNotifyGive(s_wifi_reconnect_task_handle);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Always clear both bits on disconnect
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        if (s_retry_num < MAXIMUM_RETRY) {
            xTaskNotifyGive(s_wifi_reconnect_task_handle);
            s_retry_num++;
            ESP_LOGI(TAG, "Signal reconnect task (retry %d)...", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_start();
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Always clear both first (safety, avoids stale bits)
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Wi-Fi reconnect task. Only reconnects when signaled.
 * Takes the sensor mutex before reconnecting to avoid interference with critical sensor tasks.
 */
void wifi_reconnect_task(void *pvParameter) {
    while (1) {
        // Wait for notification from event handler
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Take the sensor mutex before reconnecting Wi-Fi
        if (xSensorMutex != NULL) {
            if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(2000))) {
                ESP_LOGI(TAG, "wifi_reconnect_task: esp_wifi_connect()");
                esp_wifi_connect();
                xSemaphoreGive(xSensorMutex);
            } else {
                ESP_LOGW(TAG, "wifi_reconnect_task: Failed to acquire sensor mutex!");
            }
        } else {
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // Initialize the sensor mutex before anything else
    xSensorMutex = xSemaphoreCreateMutex();
    if (xSensorMutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor mutex!");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Start the Wi-Fi reconnect task (mutex will be initialized in app_main)
    xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 4096, NULL, 10, &s_wifi_reconnect_task_handle);

    /* Wait for connection result */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to network SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}