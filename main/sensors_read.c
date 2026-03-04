
#include "headers.h"

const char *TAGs = "SENSORS_READ";
SemaphoreHandle_t xSensorMutex = NULL; // Make sure this is declared in your sensors_read.c

void sensors_reading_task(void *pvParameters){
    float temperature = 0.0, humidity = 0.0, pressure = 0.0;
    measurement_t co2_value;
    measurement_q_t measurement;
    EventBits_t bits;
    static const TickType_t data_publish_interval = pdMS_TO_TICKS(60000);
    
    while(1){
        // Block until Wi-Fi is connected
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        // Get sensors readings every 60s
        vTaskDelay(data_publish_interval);

        // Try to take the sensor mutex before accessing any critical sensor resource
        if (xSensorMutex != NULL) {
            if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(2000))) {

                // --- Begin critical section: all sensor reads below are thread-safe ---

                // Get BME280 measurements
                bme280_read_temperature(bme280, &temperature);
                bme280_read_humidity(bme280, &humidity);
                bme280_read_pressure(bme280, &pressure);
                ESP_LOGI(TAGs, "Temperature: %.3f °C", temperature);

                measurement.temperature = temperature;
                measurement.humidity = humidity;
                measurement.pressure = pressure;

                // Get CO2 reading
                co2_value = getMeasurement(UART_MHZin);
                measurement.co2 = co2_value.co2_ppm;
                ESP_LOGI(TAGs, "CO2 level: %d ppm", co2_value.co2_ppm);

                // Get light sensor status
                int light = gpio_get_level(GPIO_LIGHT);
                measurement.light_state = (light == 0);
                ESP_LOGI(TAGs, "Light sensor input is (%d)", light == 0);      

                //Send the data to the measurement queue
                xQueueSend(measurement_queue, &measurement, portMAX_DELAY);

                // --- End of critical section ---
                xSemaphoreGive(xSensorMutex);
            } else {
                ESP_LOGW(TAGs, "sensors_reading_task: Failed to acquire sensor mutex!");
            }
        }
    }
}
