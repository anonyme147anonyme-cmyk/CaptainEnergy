#include "Arduino.h"
#include "Wire.h"
SerialMock Serial; TwoWire Wire;
#include "../firmware/collect_grid/src/main.cpp"
int main(){ setup(); for(int i=0;i<10;i++) loop(); return 0; }
