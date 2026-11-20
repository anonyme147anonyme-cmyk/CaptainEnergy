#include "Arduino.h"
#include "Wire.h"
#include "WiFi.h"
SerialMock Serial; TwoWire Wire; WiFiMock WiFi;
#include "../firmware/edge_energy_manager/src/main.cpp"
int main(){ setup(); for(int i=0;i<10;i++) loop(); return 0; }
