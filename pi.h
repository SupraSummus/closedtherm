// A PI controller with a clamped output, kept free of the sketch: it is handed
// the time and the reading, and called by the state it is in, so it holds no
// opinion about boilers and can be tested on its own. pi_source.h picks the
// state; ardu.ino owns the one instance.
#pragma once

#include <stdint.h>

struct PIController {
    // Tuning. Starting points rather than tuned values.
    float kp = 8.0;    // output units per unit of error
    float ki = 0.002;  // ... per unit of error per second
    float target = 21.0;
    // The output range, shared rather than per instance so pi_source.h can bound
    // the integral by the same two numbers instead of repeating them.
    static constexpr float outMin = 5.0;
    static constexpr float outMax = 80.0;

    // Worked out by the three below, and reported as they stand.
    float error = 0.0;
    float integral = outMin;
    float output = outMin;
    uint32_t lastUpdate = 0;

    // Starts the clock, so the first advance has a real interval behind it
    // rather than everything since zero.
    void start(uint32_t now) {
        lastUpdate = now;
    }

    // Three ways to advance to now, one per state the controller can be in. They
    // differ only in what becomes of the integral, and each takes exactly what it
    // reads, so there is no argument to pass that the state will ignore.

    // In charge of something that can answer the error: integrate it.
    void drive(float measured, uint32_t now) {
        error = target - measured;
        integral = clamp(integral + ki * error * ((now - lastUpdate) / 1000.0f));
        lastUpdate = now;
        output = clamp(integral + kp * error);
    }

    // In charge, but nothing can act on the output: banking the error would only
    // pile up a demand nobody asked for, so leave the integral where it is.
    void hold(float measured, uint32_t now) {
        error = target - measured;
        lastUpdate = now;
        output = clamp(integral + kp * error);
    }

    // Something else is in charge, `driven` being what it asks for. Integrating an
    // error this output cannot answer would leave the integral wherever the reading
    // last stopped moving it, and taking over would then jump the plant to that.
    // Sit where the output matches what is really going out instead, so taking over
    // changes nothing at that instant — as far as the range reaches, anyway.
    void track(float measured, float driven, uint32_t now) {
        error = target - measured;
        integral = clamp(driven - kp * error);
        lastUpdate = now;
        output = clamp(integral + kp * error);
    }

    // Clamping the integral to the output range, rather than to zero, is what
    // stops it winding up while the output sits at a limit.
    float clamp(float value) const {
        if (value < outMin) {
            return outMin;
        }
        if (value > outMax) {
            return outMax;
        }
        return value;
    }
};
