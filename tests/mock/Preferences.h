#pragma once
struct Preferences { bool begin(const char*, bool){return true;} float getFloat(const char*, float d){return d;} size_t putFloat(const char*, float){return 4;} int getInt(const char*, int d){return d;} size_t putInt(const char*, int){return 4;} };
