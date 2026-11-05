#ifndef HEADERS_H
#define HEADERS_H

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>
#include <esp_timer.h>
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_tls.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "bme280.h"
#include "MHZ19.h"

#include <cJSON.h>

// Temperature sensor BME280
extern bme280_handle_t bme280;
#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

// Measurement queue (length 1, written with xQueueOverwrite: always holds the latest sample)
extern QueueHandle_t measurement_queue;

// Wi-Fi
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1   // set after MAXIMUM_RETRY consecutive failures (reconnection keeps going)
#define MAXIMUM_RETRY      7
extern EventGroupHandle_t s_wifi_event_group;

// Sampling / publishing period
#define DATA_PUBLISH_INTERVAL_MS   60000

// Safety net: reboot if no OMF message was accepted for this long (seconds).
// Recovers from any unforeseen stuck state (network stack, TLS, sensors...).
#define PUBLISH_STALL_RESTART_S    (20 * 60)

// Task priorities (keep below the Wi-Fi / lwIP / event-loop tasks, which run at 18-23)
#define PRIO_SENSORS        5
#define PRIO_TOKEN_REFRESH  4
#define PRIO_OMF_PUBLISH    4
#define PRIO_WIFI_RECONNECT 6
#define PRIO_HEALTH         2

// One sample of every sensor. *_valid flags let the publisher skip bad readings
// instead of sending garbage (e.g. CO2 = 65535) that the OMF endpoint rejects.
typedef struct {
    float    temperature;
    float    pressure;
    float    humidity;
    bool     bme_valid;
    int16_t  co2;
    bool     co2_valid;
    bool     light_state;
} measurement_q_t;

//UART pins for CO2 out sensor
#define TXDi_PIN (GPIO_NUM_23)
#define RXDi_PIN (GPIO_NUM_19)
#define UART_MHZin (UART_NUM_1)

// Define light sensor input
#define GPIO_LIGHT (GPIO_NUM_35)

// Define button input
#define GPIO_BUTTON (GPIO_NUM_27)

// Functions
void uart_init(uart_port_t UART_num, int RXD_PIN, int TXD_PIN);
void gpio_init(void);
void i2c_master_init(void);
void http_client_app_start(void);
void time_sync_init(void);
void wifi_init_sta(void);
uint32_t omf_seconds_since_last_success(void);

// Tasks
void sensors_reading_task(void *pvParameters);
void omf_publish_task(void *pvParameters);
void token_refresh_task(void *pvParameters);
void wifi_reconnect_task(void *pvParameter);

#endif
