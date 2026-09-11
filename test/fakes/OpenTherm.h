// Boiler that never answers: every exchange times out, every reading is zero.
#pragma once

enum class OpenThermResponseStatus { NONE, SUCCESS, INVALID, TIMEOUT };

class OpenTherm {
public:
    OpenTherm(int, int, bool = false) {}
    void begin(void (*)()) {}
    void handleInterrupt() {}

    unsigned long setBoilerStatus(bool ch, bool dhw, bool = false, bool = false, bool = false) {
        asked_central_heating = ch;
        asked_hot_water = dhw;
        return 0;
    }
    OpenThermResponseStatus getLastResponseStatus() { return OpenThermResponseStatus::TIMEOUT; }
    bool isCentralHeatingActive(unsigned long) { return false; }
    bool isHotWaterActive(unsigned long) { return false; }
    bool isFlameOn(unsigned long) { return false; }
    bool setBoilerTemperature(float t) { ch_setpoints++; last_ch_setpoint = t; return false; }
    bool setDHWSetpoint(float t) { dhw_setpoints++; last_dhw_setpoint = t; return false; }
    float getBoilerTemperature() { return 0; }
    float getPressure() { return 0; }
    float getReturnTemperature() { return 0; }
    float getModulation() { return 0; }
    float getDHWTemperature() { return 0; }
    unsigned char getFault() { return 0; }
    static const char* statusToString(OpenThermResponseStatus) { return "TIMEOUT"; }

    // Test side: what the last exchange asked the boiler for.
    bool asked_central_heating = false;
    bool asked_hot_water = false;

    // Setpoint writes the test can count.
    int ch_setpoints = 0;
    int dhw_setpoints = 0;
    float last_ch_setpoint = 0;
    float last_dhw_setpoint = 0;
};
