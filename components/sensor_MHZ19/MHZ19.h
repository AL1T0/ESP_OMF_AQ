/*
  MHZ19.h - MH-Z19 CO2 sensor library for ESP-WROOM-02/32(ESP8266/ESP32) or Arduino
  version 1.0
  
  License MIT
*/

#ifndef _MHZ19
#define _MHZ19

#include "driver/uart.h"

//#include "Arduino.h"
//#include "SoftwareSerial.h"

enum MHZ19_UART_DATA
{
	PPM,
	TEMPERATURE,
	STAT
};

enum MHZ19_PWM_DATA
{
	CALC_2000_PPM,
	CALC_5000_PPM
};

enum MHZ19_POTOCOL
{
	UART,
	PWM
};

typedef struct measurement {
	int co2_ppm;
	int temperature;
	int state;
} measurement_t;


typedef struct
{
// 	void begin(int rx, int tx);
// 	void begin(int pwm);
// 	void setAutoCalibration(boolean autocalib);
// 	void calibrateZero();
// 	void calibrateSpan(int ppm);
// 	int getStatus();
// 	measurement_t getMeasurement();
// 	//int getPpmPwm();

// 	boolean isWarming();

//   protected:
// 	//void writeCommand(uint8_t com[]);
// 	//void writeCommand(uint8_t com[], uint8_t response[]);

//   private:
// 	uint8_t mhz19_checksum(uint8_t com[]);
// 	measurement_t getSerialData();
// 	void setPwmData(MHZ19_PWM_DATA type);



// 	// serial command

	
// 	// Pwm Pin
// 	//int _pwm_pin;

// 	// Pwm Data Flag
// 	// uint8_t PWM_DATA_SELECT = MHZ19_PWM_DATA::CALC_2000_PPM;
} MHZ19_t;

measurement_t getMeasurement(uint8_t port);

#endif
