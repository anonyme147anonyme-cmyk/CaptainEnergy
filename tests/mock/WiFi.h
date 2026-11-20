#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
struct WiFiMock { int status(){return 0;} void mode(int){} void begin(const char*,const char*){} };
extern WiFiMock WiFi;
struct Client {}; struct WiFiClient : Client {};
