#ifndef HEADERS_H
#define HEADERS_H

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>
#include <esp_timer.h>
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
#include "protocol_examples_common.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "bme280.h"
#include "MHZ19.h"

#include <cJSON.h>

// HTTP Client
#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

// Temperature sensor BME280
extern bme280_handle_t bme280;
#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22
#define I2C_MASTER_NUM I2C_NUM_0 
#define I2C_MASTER_FREQ_HZ 100000 
#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

// Define queues and semaphores
extern QueueHandle_t measurement_queue;
extern SemaphoreHandle_t xSensorMutex;

// Wi-Fi 
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAXIMUM_RETRY      7
extern EventGroupHandle_t s_wifi_event_group;

// Define a structure to hold measurement data
typedef struct {
    float temperature;
    float pressure;
    float humidity;
    uint16_t co2;
    bool light_state;
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

// Tasks
void sensors_reading_task(void *pvParameters);
void omf_publish_task(void *pvParameters);
void token_refresh_task(void *pvParameters);
//void time_daily_resync_task(void *pvParameters);
void wifi_reconnect_task(void *pvParameter);

#endif