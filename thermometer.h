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

    // Two independent filters, against two kinds of disturbance. README.md has
    // the reasoning behind each.
    //
    // A median over the last `window` readings, one per loop() pass, against
    // errors in the reading: supply and reference noise, which lands as short
    // dips. The window is measured in passes, not seconds.
    static constexpr int window = 32;
    float readings[window];
    int next = 0;  // slot the next reading goes into
    int held = 0;  // slots filled so far, until the buffer wraps

    // A first-order low-pass over the median, against the room itself moving
    // briefly, which the median has no reason to drop. Its time constant is in
    // seconds off millis(), not in passes.
    static constexpr float timeConstant = 60.0;
    unsigned long lastSample = 0;

    // As of the last pass. millivolts and celsius are what the thermometer
    // says: the median, low-passed.
    float median = 0.0;
    float millivolts = 0.0;
    float celsius = 0.0;

    explicit Thermometer(int pin) : pin(pin) {}

    void begin() {
        pinMode(pin, INPUT);
        analogSetPinAttenuation(pin, ADC_0db);  // 0-1 V range
    }

    // One pass: the reading and the time, both off the board.
    void sample() {
        unsigned long now = millis();
        readings[next] = analogReadMilliVolts(pin);
        next = (next + 1) % window;
        if (held < window) {
            held++;
        }
        median = medianOfWindow();

        if (held == 1) {
            millivolts = median;  // seeded, so a boot starts at the room and not at zero
        } else {
            // dt / (tau + dt) rather than dt / tau: stays put at dt = 0 and
            // never overshoots, however long the pass took.
            float dt = (now - lastSample) / 1000.0f;
            millivolts += (median - millivolts) * dt / (timeConstant + dt);
        }
        lastSample = now;
        celsius = toCelsius(millivolts);
    }

    // Over the readings taken so far, so the first values after a boot are not
    // dragged towards zero by the empty slots.
    float medianOfWindow() const {
        float sorted[window];
        std::copy(readings, readings + held, sorted);
        std::sort(sorted, sorted + held);
        return held % 2 ? sorted[held / 2] : (sorted[held / 2 - 1] + sorted[held / 2]) / 2;
    }

    static float toCelsius(float millivolts) {
        return referenceCelsius - (millivolts - referenceMillivolts) / millivoltsPerDegree;
    }

    // The other direction, for pointing a fake sensor at a temperature.
    static float toMillivolts(float celsius) {
        return referenceMillivolts - (celsius - referenceCelsius) * millivoltsPerDegree;
    }
};
