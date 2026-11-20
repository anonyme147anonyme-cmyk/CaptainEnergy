#pragma once
#include "Wire.h"
#define SSD1306_SWITCHCAPVCC 2
#define SSD1306_WHITE 1
struct Adafruit_SSD1306 { Adafruit_SSD1306(int,int,TwoWire*,int){} bool begin(int,uint8_t){return true;} void clearDisplay(){} void setTextSize(int){} void setTextColor(int){} void setCursor(int,int){} int printf(const char*, ...){return 0;} void display(){} };
