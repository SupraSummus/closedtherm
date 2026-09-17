// A PI controller with a clamped output, kept free of the sketch: it is handed
// the time and the reading, and called by the state it is in, so it holds no
// opinion about boilers and can be tested on its own. pi_source.h picks the
// state; ardu.ino owns the one instance.
//
// The one thing it does assume about its plant is that the output and the reading
// are the same quantity — a flow temperature driving a room temperature — which is
// what lets clampIntegral() put the reading under the integral. Everything else
// here is arithmetic.
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
        integral = clampIntegral(integral + ki * error * ((now - lastUpdate) / 1000.0f), measured);
        lastUpdate = now;
        output = outputFor(measured);
    }

    // In charge, but nothing can act on the output: banking the error would only
    // pile up a demand nobody asked for, so this does not integrate. The floor
    // still applies, being a bound on what the integral can mean rather than
    // something the error does to it.
    void hold(float measured, uint32_t now) {
        error = target - measured;
        integral = clampIntegral(integral, measured);
        lastUpdate = now;
        output = outputFor(measured);
    }

    // Something else is in charge, `driven` being what it asks for. Integrating an
    // error this output cannot answer would leave the integral wherever the reading
    // last stopped moving it, and taking over would then jump the plant to that.
    // Sit where the output matches what is really going out instead, so taking over
    // changes nothing at that instant — as far as the range reaches, anyway.
    void track(float measured, float driven, uint32_t now) {
        error = target - measured;
        integral = clampIntegral(driven - kp * error, measured);
        lastUpdate = now;
        output = outputFor(measured);
    }

    // The output a reading gives against the integral as it stands, which is where
    // all three states end up. Answering it without advancing anything is what
    // lets a caller decide between them first, so it has to apply the same floor
    // they do or it would answer for an integral none of them would have left.
    float outputFor(float measured) const {
        return clamp(clampIntegral(integral, measured) + kp * (target - measured));
    }

    // The integral is the output the plant needs before the error moves it, so
    // where output and reading are the same quantity, a value under the reading is
    // not a small base but one that cannot act: at zero error the output would come
    // out under the reading too, and nothing the controller does from there raises
    // it. That is the floor, and the cost of it is the range below the reading,
    // which is why track() cannot follow a driven value down there.
    float clampIntegral(float value, float measured) const {
        float floor = measured > outMin ? measured : outMin;
        return clamp(value > floor ? value : floor);
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
