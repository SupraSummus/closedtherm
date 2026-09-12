// The room thermometer: a transistor junction on an analog pin, read the way it
// needs reading and turned into degrees. ardu.ino owns the one instance and
// tells it the pin.
#pragma once

#include <Arduino.h>
#include <algorithm>

struct Thermometer {
    int pin;

    // Calibration. A starting point rather than a measured one: the junction
    // drops about 2 mV for every degree it warms.
    static constexpr float referenceMillivolts = 671.0;
    static constexpr float referenceCelsius = 18.0;
    static constexpr float millivoltsPerDegree = 2.0;

    // One reading per pass, the last this many kept, and their median reported.
    // A pass is one loop() iteration, so the readings are spread over that many
    // iterations: a short supply dip lands on one of them or none, and the
    // median drops it however long it lasted, where an average would follow it.
    // The window is measured in iterations, not seconds.
    static constexpr int window = 64;
    float readings[window];
    int next = 0;  // slot the next reading goes into
    int held = 0;  // slots filled so far, until the buffer wraps

    // As of the last pass: the median of the window, and that in degrees.
    float millivolts = 0.0;
    float celsius = 0.0;

    explicit Thermometer(int pin) : pin(pin) {}

    void begin() {
        pinMode(pin, INPUT);
        analogSetPinAttenuation(pin, ADC_0db);  // 0-1 V range
    }

    void sample() {
        readings[next] = analogReadMilliVolts(pin);
        next = (next + 1) % window;
        if (held < window) {
            held++;
        }
        // Over the readings taken so far, so the first values after a boot are
        // not dragged towards zero by the empty slots.
        float sorted[window];
        std::copy(readings, readings + held, sorted);
        std::sort(sorted, sorted + held);
        millivolts = held % 2 ? sorted[held / 2] : (sorted[held / 2 - 1] + sorted[held / 2]) / 2;
        celsius = toCelsius(millivolts);
    }

    static float toCelsius(float millivolts) {
        return referenceCelsius - (millivolts - referenceMillivolts) / millivoltsPerDegree;
    }

    // The other direction, for pointing a fake sensor at a temperature.
    static float toMillivolts(float celsius) {
        return referenceMillivolts - (celsius - referenceCelsius) * millivoltsPerDegree;
    }
};
