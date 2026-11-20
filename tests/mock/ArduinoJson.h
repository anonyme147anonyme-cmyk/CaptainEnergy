#pragma once
#include <cstddef>
#include <cstdint>
struct JsonVariant {
  template<class T> bool is() const { return false; }
  template<class T> T as() const { return T(); }
  int operator|(int d) const { return d; }
  template<class T> JsonVariant& operator=(const T&) { return *this; }
};
struct JsonDocument { JsonVariant v; JsonVariant& operator[](const char*) { return v; } };
struct DeserializationError { operator bool() const { return false; } };
inline DeserializationError deserializeJson(JsonDocument&, const uint8_t*, size_t){ return {}; }
inline size_t serializeJson(const JsonDocument&, char*, size_t){ return 0; }
