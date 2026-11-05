#include "headers.h"
#include <math.h>

static const char *TAGs = "SENSORS_READ";

// MH-Z19 measuring range is 0-5000 ppm (some variants 10000); anything else is a bad frame
#define CO2_MIN_PPM  0
#define CO2_MAX_PPM  10000

// Reads all sensors every DATA_PUBLISH_INTERVAL_MS and hands the sample to the
// publisher. It never blocks on the network: xQueueOverwrite keeps only the
// latest sample, so a slow or offline publisher can't stall this task.
void sensors_reading_task(void *pvParameters)
{
    const TickType_t period = pdMS_TO_TICKS(DATA_PUBLISH_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        // Fixed-rate schedule (no drift from the time spent reading)
        vTaskDelayUntil(&last_wake, period);

        measurement_q_t m = {0};

        // BME280
        float t = 0, h = 0, p = 0;
        esp_err_t e1 = bme280_read_temperature(bme280, &t);
        esp_err_t e2 = bme280_read_humidity(bme280, &h);
        esp_err_t e3 = bme280_read_pressure(bme280, &p);
        m.bme_valid = (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK &&
                       isfinite(t) && isfinite(h) && isfinite(p)); // NaN would break the JSON
        m.temperature = t;
        m.humidity    = h;
        m.pressure    = p;
        if (m.bme_valid) {
            ESP_LOGI(TAGs, "Temperature: %.2f C, Humidity: %.2f %%, Pressure: %.2f", t, h, p);
        } else {
            ESP_LOGW(TAGs, "BME280 read failed (%s/%s/%s)",
                     esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
        }

        // MH-Z19 (CO2)
        measurement_t co2 = getMeasurement(UART_MHZin);
        m.co2_valid = (co2.co2_ppm >= CO2_MIN_PPM && co2.co2_ppm <= CO2_MAX_PPM);
        m.co2 = m.co2_valid ? (int16_t)co2.co2_ppm : 0;
        if (m.co2_valid) {
            ESP_LOGI(TAGs, "CO2 level: %d ppm", m.co2);
        } else {
            ESP_LOGW(TAGs, "CO2 read failed");
        }

        // Light sensor (active low)
        m.light_state = (gpio_get_level(GPIO_LIGHT) == 0);
        ESP_LOGI(TAGs, "Light: %d", m.light_state);

        xQueueOverwrite(measurement_queue, &m);
    }
}
