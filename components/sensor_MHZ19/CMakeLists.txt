cmake_minimum_required(VERSION 3.16)

idf_component_register(SRCS
                                        "MHZ19.c"
                    INCLUDE_DIRS .
                    REQUIRES 
                                        nvs_flash
                                        esp_event
                                        driver
                                        esp_driver_uart
                                        esp_netif
                                        esp_hw_support
                                                        )
