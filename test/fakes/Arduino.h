// Host stand-in for the Arduino core: just enough for ardu.ino to compile and run on a PC.
#pragma once
#include <stdint.h>
#include <cstdio>
#include <string>

#define IRAM_ATTR
#define INPUT 0
enum { ADC_0db };

namespace fake {
inline uint32_t millis = 0;
inline int adc_mv = 671;
}

inline unsigned long millis() { return fake::millis; }
inline void pinMode(int, int) {}
inline void analogSetPinAttenuation(int, int) {}
inline int analogReadMilliVolts(int) { return fake::adc_mv; }

class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned int v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(float v) { char b[32]; snprintf(b, sizeof b, "%.2f", v); s_ = b; }
    String(double v) { char b[32]; snprintf(b, sizeof b, "%.2f", v); s_ = b; }

    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    float toFloat() const { return strtof(s_.c_str(), nullptr); }
    bool operator==(const char* o) const { return s_ == o; }
    String operator+(const String& o) const { return String((s_ + o.s_).c_str()); }
    bool concat(const char* s) { s_ += s; return true; }

private:
    std::string s_;
};
inline String operator+(const char* a, const String& b) { return String(a) + b; }

struct FakeSerial {
    void begin(unsigned long) {}
    void println(const String&) {}
    void println(const char*) {}
};
inline FakeSerial Serial;
