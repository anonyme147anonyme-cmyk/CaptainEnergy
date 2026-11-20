#pragma once
#include "WiFi.h"
struct PubSubClient { PubSubClient(Client&){} void setServer(const char*, int){} void setCallback(void(*)(char*, uint8_t*, unsigned int)){} bool setBufferSize(int){return true;} bool connected(){return false;} bool connect(const char*){return true;} bool subscribe(const char*){return true;} bool loop(){return true;} bool publish(const char*, const char*){return true;} bool publish(const char*, const uint8_t*, unsigned int, bool){return true;} };
