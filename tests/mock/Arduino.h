#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#define ESP_ARDUINO_VERSION_MAJOR 3
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
typedef uint8_t byte;
inline unsigned long millis(){ return 0; }
inline unsigned long micros(){ return 0; }
inline void delay(unsigned long){}
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}
inline int digitalRead(int){ return 0; }
inline bool ledcAttach(int,int,int){ return true; }
inline void ledcWrite(int,int){}
inline unsigned analogReadMilliVolts(int){ return 1650; }
inline void configTzTime(const char*, const char*){}
class String {
 public:
  std::string s;
  String(){}
  String(const char* c):s(c){}
  String(const std::string& x):s(x){}
  String(int v){ s=std::to_string(v);} 
  String(unsigned int v){ s=std::to_string(v);} 
  String(long v){ s=std::to_string(v);} 
  String(char c){ s=std::string(1,c);} 
  String(float v, int d=2){ char b[32]; snprintf(b,32,"%.*f",d,v); s=b; }
  String(double v, int d=2){ char b[32]; snprintf(b,32,"%.*f",d,v); s=b; }
  String& operator+=(const String& o){ s+=o.s; return *this; }
  String& operator+=(char c){ s+=c; return *this; }
  bool operator==(const String& o) const { return s==o.s; }
  bool operator==(const char* o) const { return s==o; }
  bool startsWith(const char* p) const { return s.rfind(p,0)==0; }
  String substring(int a) const { return String(s.substr(a)); }
  void trim(){ while(!s.empty() && isspace(s.back())) s.pop_back(); while(!s.empty()&&isspace(s.front())) s.erase(0,1);} 
  unsigned length() const { return s.size(); }
  float toFloat() const { return atof(s.c_str()); }
  const char* c_str() const { return s.c_str(); }
};
inline String operator+(const String& a, const String& b){ return String(a.s+b.s); }
inline String operator+(const char* a, const String& b){ return String(std::string(a)+b.s); }
inline String operator+(const String& a, const char* b){ return String(a.s+b); }
struct SerialMock {
  void begin(int){} void setTimeout(int){}
  template<class T> void print(T){} template<class T> void print(T,int){}
  void println(){} template<class T> void println(T){}
  int printf(const char*, ...) { return 0; }
  int available(){ return 0; }
  String readStringUntil(char){ return String(); }
};
extern SerialMock Serial;
struct EspMock { unsigned getFreeHeap(){return 0;} }; static EspMock ESP;
