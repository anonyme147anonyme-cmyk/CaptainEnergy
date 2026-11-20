#pragma once
#include <cstdint>
struct Adafruit_INA219 { Adafruit_INA219(uint8_t){} bool begin(){return true;} void setCalibration_32V_2A(){} float getBusVoltage_V(){return 11.7f;} float getShuntVoltage_mV(){return 30;} float getCurrent_mA(){return 3000;} };
