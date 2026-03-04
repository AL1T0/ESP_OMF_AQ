/*
  MHZ19.cpp - MH-Z19 CO2 sensor library for ESP8266 or Arduino
  version 1.0
  
  License MIT
*/

#include "MHZ19.h"
#include <stdio.h>
#include "esp_log.h"
#include <string.h>

#define WAIT_READ_TIMES 100
#define WAIT_READ_DELAY 10

	static const int REQUEST_CNT = 8;
	static const int RESPONSE_CNT = 9;

	static const int RX_BUF_SIZE = 1024;

	uint8_t getppm[] = {0xff, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t zerocalib[] = {0xff, 0x01, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t spancalib[] = {0xff, 0x01, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t autocalib_on[] = {0xff, 0x01, 0x79, 0xA0, 0x00, 0x00, 0x00, 0x00};
	uint8_t autocalib_off[] = {0xff, 0x01, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00};

void writeCommand(uint8_t cmd[], uint8_t port);
void writeCommand_wait_resp(uint8_t cmd[], uint8_t *response, uint8_t port);
//measurement_t getMeasurement(void);
uint8_t mhz19_checksum(uint8_t com[]);

void setAutoCalibration(bool autocalib, uint8_t port)
{
	writeCommand(autocalib ? autocalib_on : autocalib_off, port);
}

void calibrateZero(uint8_t port)
{
	writeCommand(zerocalib, port);
}

void calibrateSpan(int ppm, uint8_t port)
{
	if (ppm < 1000)
		return;

	uint8_t cmd[REQUEST_CNT];
	for (int i = 0; i < REQUEST_CNT; i++)
	{
		cmd[i] = spancalib[i];
	}
	cmd[3] = (uint8_t)(ppm / 256);
	cmd[4] = (uint8_t)(ppm % 256);
	writeCommand(cmd, port);
}

/* undocumented function */
int getStatus(uint8_t port)
{
	measurement_t m = getMeasurement(port);
	return m.state;
}

/* undocumented function, seems not to work with MH-Z19B */
bool isWarming(uint8_t port)
{
	return (getStatus(port)<= 1);
}

//protected
void writeCommand(uint8_t cmd[], uint8_t port)
{
	writeCommand_wait_resp(cmd, NULL, port);
}

void writeCommand_wait_resp(uint8_t cmd[], uint8_t *response, uint8_t port)
{
	int txBytes;
	txBytes = uart_write_bytes(port, cmd, REQUEST_CNT);
	uint8_t checksum = mhz19_checksum(cmd);
	txBytes = uart_write_bytes(port, &checksum, (size_t)1);

	if (response != NULL)
	{
		int i = 0;

		// LECTURA UART EN MODO ESPERA BLOQUEANTE, POOLING
		static const char *RX_TASK_TAG = "RX_BLOCK";
		esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
		uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE+1);
		while (i < WAIT_READ_TIMES) {
			const int rxBytes = uart_read_bytes(port, data, RX_BUF_SIZE, WAIT_READ_DELAY / portTICK_PERIOD_MS);
			if (rxBytes > 0) {
				data[rxBytes] = 0;
				//ESP_LOGI(RX_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);
				//ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_INFO);
				memcpy(response, data, RESPONSE_CNT);
				//printf("info: sensor data received.");
				free(data);
				return;
			} else {
				i++;
			}
		}
		free(data);

				printf("error: can't get MH-Z19 response.");
				return;
	}
}

//private

measurement_t getMeasurement(uint8_t port)
{
	uint8_t buf[RESPONSE_CNT];
	for (int i = 0; i < RESPONSE_CNT; i++)
	{
		buf[i] = 0x0;
	}

	writeCommand_wait_resp(getppm, buf, port);
	// parse
	measurement_t measurement = {};
	if (buf[0] == 0xff && buf[1] == 0x86 && mhz19_checksum(buf) == buf[RESPONSE_CNT - 1])
	{
		measurement.co2_ppm = buf[2] * 256 + buf[3];
		measurement.temperature = buf[4] - 40;
		measurement.state = buf[5];
	}
	else
	{
		measurement.co2_ppm = measurement.temperature = measurement.state = -1;
	}
	return measurement;
}

uint8_t mhz19_checksum(uint8_t com[])
{
	uint8_t sum = 0x00;
	for (int i = 1; i < REQUEST_CNT; i++)
	{
		sum += com[i];
	}
	sum = 0xff - sum + 0x01;
	return sum;
}
