#pragma once
#include <cstdint>
#include "OneWire.h"
typedef uint8_t DeviceAddress[8];
struct DallasTemperature { DallasTemperature(OneWire*){} void begin(){} void setWaitForConversion(bool){} void requestTemperatures(){} int getDeviceCount(){return 3;} float getTempCByIndex(int){return 4;} bool getAddress(uint8_t*, int){return true;} float getTempC(const uint8_t*){return 4;} };
