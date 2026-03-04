#include "headers.h"

static const int RX_BUF_SIZE = 1024;

//CO2 readings
void uart_init(uart_port_t UART_num, int RXD_PIN, int TXD_PIN) {
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // We won't use a buffer for sending data.
    uart_driver_install(UART_num, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_num, &uart_config);
    uart_set_pin(UART_num, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}