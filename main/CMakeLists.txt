# Embed the server root certificate into the final binary
#
# (If this was a component, we would set COMPONENT_EMBED_TXTFILES here.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

idf_component_register(
    SRCS "wifi_handler.c" "app_main.c" "uart.c" "gpio.c" "sensors_read.c" "http_client.c" "time_sync.c"
    INCLUDE_DIRS "."
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")