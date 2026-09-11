// In-memory NVS. Tests can pre-seed and inspect the maps directly.
#pragma once
#include <stdint.h>
#include <map>
#include <string>

class Preferences {
public:
    bool begin(const char*, bool = false) { return true; }
    void end() {}

    size_t putBool(const char* k, bool v) { bools[k] = v; return 1; }
    size_t putFloat(const char* k, float v) { floats[k] = v; return 4; }
    size_t putUInt(const char* k, uint32_t v) { uints[k] = v; return 4; }

    bool getBool(const char* k, bool d = false) { return bools.count(k) ? bools[k] : d; }
    float getFloat(const char* k, float d = 0) { return floats.count(k) ? floats[k] : d; }
    uint32_t getUInt(const char* k, uint32_t d = 0) { return uints.count(k) ? uints[k] : d; }

    std::map<std::string, bool> bools;
    std::map<std::string, float> floats;
    std::map<std::string, uint32_t> uints;
};
