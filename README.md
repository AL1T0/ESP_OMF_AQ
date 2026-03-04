| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- |

# ESP32 Air Quality IoT Sensor
This application connects to a wireless network and publishes environmental data to the CONNECT Data Services platform by AVEVA.

It reads temperature, humidity, and pressure from a BME280 sensor, carbon dioxide concentration from an MHZ19 sensor, and digital light status. The data is gathered every 60 seconds and then published using the OMF format over HTTP with an OAuth2 authentication flow.

## Wiring
An external 5 V power supply is recommended for the carbon dioxide sensor.

### Wiring the BME280 sensor
- VCC to 3.3 V
- GND to GND
- SDA to GPIO 21
- SCL to GPIO 22

### Wiring the MHZ19 sensor
- Vin to 5 V
- GND to GND
- Tx to GPIO 19
- Rx to GPIO 23

### Wiring the Light sensor
- VCC to 3.3 V
- GND to GND
- Data to GPIO 35

## Used Libraries
### HTTP Client and OMF
This application utilizes the ESP IDF HTTP Client to manage POST requests. It implements an OAuth2 client credentials flow to obtain a bearer token and sends JSON payloads formatted according to the Open Message Format specification by OSIsoft-now-AVEVA. Registration of OMF types and containers is done automatically and persisted in the non volatile storage.

### BME280 Sensor
To read atmospheric data it uses a standard I2C driver implementation for the BME280 sensor.

### MHZ19 Sensor
To read CO2 it relies on UART communication parsing the serial output of the MHZ19 module.

## How to use
### Hardware Required
This program was designed to run on a standard ESP32 development board.

### Configure the project
* Create a file named keys.h in your project directory based on the provided keys_example.h file.
* Populate the file with your specific wireless network credentials and your AVEVA CONNECT Data Services client ID and client secret.
* Ensure the OMF endpoint URL matches your tenant and namespace.

### Build and Flash
Build the project and flash it to the board, then run the monitor tool to view the serial output. Run the standard ESP IDF build command in your terminal. For example, idf.py flash monitor.

### Example Output
```
I (3714) WI FI station: ESP WIFI MODE STA
I (3964) WI FI station: Connected to network SSID:YOURSSID
I (4164) TIME SYNC: Initializing SNTP
I (4174) TIME SYNC: Time synchronized: 2026/03/04T12:00:00Z
I (4184) SENSORS READ: Temperature: 24.500 C
I (4314) SENSORS READ: CO2 level: 450 ppm
I (4484) HTTP HANDLER: Token request
I (4684) HTTP HANDLER: Token OK
I (4884) HTTP HANDLER: OMF registration complete
I (5194) HTTP HANDLER: OMF data status: 202
```