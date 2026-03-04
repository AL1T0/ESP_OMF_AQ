#include "headers.h"

bme280_handle_t bme280 = NULL;

//Function to initialize I/O
void gpio_init() {
    // Initialize the input of the pad button
    //gpio_install_isr_service(0);
    //esp_rom_gpio_pad_select_gpio(GPIO_BUTTON);
    //gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
    //gpio_pullup_en(GPIO_BUTTON);
    //gpio_set_intr_type(GPIO_BUTTON, GPIO_INTR_POSEDGE);
    //gpio_isr_handler_add(GPIO_BUTTON, button_isr_handler, NULL);

    // Initialize the input of the ligh sensor
    esp_rom_gpio_pad_select_gpio(GPIO_LIGHT);
    gpio_set_direction(GPIO_LIGHT, GPIO_MODE_INPUT);
    //gpio_set_intr_type(GPIO_LIGHT, GPIO_INTR_ANYEDGE);
    //gpio_isr_handler_add(GPIO_LIGHT, sensor_isr_handler, NULL);
}

// Interruption for light sensor state change
//void IRAM_ATTR sensor_isr_handler(void* arg) {
//    int sensor_state = gpio_get_level(GPIO_LIGHT);
//}

// Interruption for button actions
//void IRAM_ATTR button_isr_handler(void* arg) {
//    int button_state = gpio_get_level(GPIO_BUTTON);
//}

// Initialize I2C bus and BME280 sensor
void i2c_master_init(){
    i2c_bus_handle_t i2c_bus = NULL;

	//Step1: Init I2C bus
	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = SDA_PIN,
		.scl_io_num = SCL_PIN,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = I2C_MASTER_FREQ_HZ,
	};
	
	i2c_bus = i2c_bus_create(I2C_MASTER_NUM, &conf);
    bme280 = bme280_create(i2c_bus, BME280_I2C_ADDRESS_DEFAULT);
    bme280_default_init(bme280);
}
