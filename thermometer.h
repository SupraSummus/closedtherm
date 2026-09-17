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
    static constexpr int window = 16;
    float readings[window];
    int next = 0;  // slot the next reading goes into
    int held = 0;  // slots filled so far, until the buffer wraps

    // A first-order low-pass over the median, against the room itself moving
    // briefly, which the median has no reason to drop. Its time constant is in
    // seconds off millis(), not in passes.
    //
    // How briefly a room has to move to be worth ignoring is a property of the
    // room, so the constant is a setting: ardu.ino takes it over HTTP and keeps
    // it in NVS. The band is what the sensor answers for, ends included, and 0
    // is the low-pass off: the weight below is then dt / (0 + dt) and the median
    // goes straight through.
    static constexpr float defaultTimeConstant = 600.0;
    static constexpr float minTimeConstant = 0.0;
    static constexpr float maxTimeConstant = 3600.0;
    float timeConstant = defaultTimeConstant;

    // How long sampling has been going, and the constant in force until it
    // reaches timeConstant: that makes the filter the plain mean so far, so the
    // first pass after a boot, which has been seen a dozen degrees off, weighs
    // one pass and not ten minutes. Accumulated rather than read off millis(),
    // so it survives the wrap, and capped at the longest constant there can be.
    float sampledFor = 0.0;
    uint32_t lastSample = 0;

    // As of the last pass. millivolts and celsius are what the thermometer
    // says: the median, low-passed.
    float median = 0.0;
    float millivolts = 0.0;
    float celsius = 0.0;

    explicit Thermometer(int pin) : pin(pin) {}

    // For what comes back from NVS, which a build with a different band may have
    // written: a negative constant would leave the low-pass weight negative or
    // unbounded rather than merely wrong. /set checks its own values instead, so
    // that a bad one is a 400 and not a silent clamp.
    void setTimeConstant(float seconds) {
        if (seconds < minTimeConstant) {
            seconds = minTimeConstant;
        }
        if (seconds > maxTimeConstant) {
            seconds = maxTimeConstant;
        }
        timeConstant = seconds;
    }

    void begin() {
        pinMode(pin, INPUT);
        analogSetPinAttenuation(pin, ADC_0db);  // 0-1 V range
    }

    // One pass: the reading and the time, both off the board.
    void sample() {
        uint32_t now = millis();
        readings[next] = analogReadMilliVolts(pin);
        next = (next + 1) % window;
        if (held < window) {
            held++;
        }
        median = medianOfWindow();

        if (held == 1) {
            millivolts = median;  // nothing to average with yet, and not zero
        } else {
            // dt / (tau + dt) rather than dt / tau: never overshoots, however
            // long the pass took. At dt = 0 it is 0 / 0 while tau is still 0.
            float dt = (now - lastSample) / 1000.0f;
            if (dt > 0) {
                sampledFor = std::min(sampledFor + dt, maxTimeConstant);
                float tau = std::min(sampledFor, timeConstant);
                millivolts += (median - millivolts) * dt / (tau + dt);
            }
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
