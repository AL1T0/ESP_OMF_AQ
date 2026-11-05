// wifi_handler.c
// Wi-Fi station with non-blocking event handling and endless reconnection
// with exponential backoff (1 s ... 30 s).
#include "headers.h"
#include "keys.h"

EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "WI-FI station";
static volatile int s_retry_num = 0;
static TaskHandle_t s_wifi_reconnect_task_handle = NULL;

#define RECONNECT_BACKOFF_MAX_MS  30000
#define BOOT_CONNECT_TIMEOUT_MS   60000

/**
 * Wi-Fi / IP event handler. Runs in the default event loop task, so it must
 * never block (no delays, no esp_wifi_stop/start here): it only updates the
 * event bits and wakes the reconnect task.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        xTaskNotifyGive(s_wifi_reconnect_task_handle);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_num++;
        if (s_retry_num >= MAXIMUM_RETRY) {
            // Lets the boot sequence continue; reconnection keeps going in background
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "Disconnected (reason %d), retry %d", ev ? ev->reason : -1, s_retry_num);
        xTaskNotifyGive(s_wifi_reconnect_task_handle);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Reconnect task: waits for a signal from the event handler, backs off, then
 * calls esp_wifi_connect(). Every failed attempt produces a new DISCONNECTED
 * event, so the loop never stops trying.
 */
void wifi_reconnect_task(void *pvParameter)
{
    while (1) {
        // Woken by the event handler; the timeout is a watchdog in case an
        // event is ever missed while disconnected.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));

        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) continue;

        int retry = s_retry_num;
        if (retry > 0) {
            uint32_t delay_ms = 1000U << MIN(retry - 1, 5); // 1,2,4,8,16,30 s
            if (delay_ms > RECONNECT_BACKOFF_MAX_MS) delay_ms = RECONNECT_BACKOFF_MAX_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            }
        }
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // The reconnect task must exist before esp_wifi_start(): the STA_START
    // event notifies it, and notifying a NULL handle crashes.
    xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 3072, NULL, PRIO_WIFI_RECONNECT,
                &s_wifi_reconnect_task_handle);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

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

    // Mains-powered device: disable modem sleep. With power save on, missed
    // beacons trigger bcn_timeout disconnects on marginal links.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Wait for the first connection (bounded, so boot never hangs here)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(BOOT_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to network SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "Not connected to SSID:%s yet; retrying in background", WIFI_SSID);
    }
}
