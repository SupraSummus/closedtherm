// The PI controller as a CH setpoint source — the layer between the arithmetic
// and the sketch. Everything specific to running a PI loop against a boiler is
// here: what its settings are called over HTTP, where they are kept, what they
// accept, which state the controller belongs in, and what / reports.
//
// pi.h below knows none of this and no more than arithmetic. ardu.ino above
// knows none of it either: it owns the sensor, the NVS namespace, the HTTP
// routes, and the switch that picks between sources.
#pragma once

#include <Arduino.h>
#include <math.h>

#include "pi.h"

// / groups these under "pi", and every name starts with that word and an
// underscore, so stripping the prefix gives the name inside the group. The one
// string is the /set parameter, the NVS key and the path in that group at once,
// which is three spellings that cannot drift apart.
//
// It does mean NVS's cap falls on the name: a key over 15 characters makes
// Preferences write nothing and return 0, which the sketch does not check, so a
// longer name would work until the reboot that quietly took it back to default.
const char piGroup[] = "pi";
const int piNameOffset = sizeof(piGroup);  // "pi" and the "_" after it, since sizeof counts the NUL

// One float this source owns: what it is called, the member it lives in, and the
// values it accepts, ends included.
struct PISetting {
    const char* name;
    float PIController::*member;
    float min;
    float max;
};

// The one setting the sketch also writes on its own, when integralDueToSave()
// says so, so its name is spelt here once for both the row and that write.
const char piIntegralName[] = "pi_integral";

// Adding a tunable is a row here and nothing else: /set takes it, / reports it,
// and setup() restores it.
const PISetting piSettings[] = {
    {"pi_target_temp", &PIController::target, 5, 35},
    {"pi_kp", &PIController::kp, 0, 100},
    {"pi_ki", &PIController::ki, 0, 100},
    // Learned rather than tuned, and here for the same reasons the rest are: so
    // that a reboot restores it, / reports it, and a tuning run can seed it
    // instead of waiting hours for it to climb.
    {piIntegralName, &PIController::integral, PIController::outMin, PIController::outMax},
};
const int piSettingCount = sizeof(piSettings) / sizeof(piSettings[0]);

// How far the integral has to move, and how long since it was last written, for
// another write to be worth it. The wait is jittered by up to the third, so the
// writes do not fall on a rigid grid.
const float piIntegralSaveEpsilon = 1.0;
const uint32_t piIntegralSaveInterval = 600000;  // 10 minutes
const uint32_t piIntegralSaveJitter = 120000;    // give or take 2

struct PISource {
    PIController control;

    // Whether this source was the one in charge as of the last update().
    bool inCharge = false;

    // What the integral was when it last went to NVS, and how long to wait before
    // it is worth writing again.
    float savedIntegral = control.integral;
    uint32_t lastIntegralSave = 0;
    uint32_t integralSaveWait = piIntegralSaveInterval;

    // The CH setpoint this source asks for.
    float setpoint() const {
        return control.output;
    }

    void start(uint32_t now) {
        control.start(now);
        lastIntegralSave = now;
        jitterIntegralSave();
    }

    void jitterIntegralSave() {
        integralSaveWait = piIntegralSaveInterval - piIntegralSaveJitter +
                           random(2 * piIntegralSaveJitter);
    }

    // True when the integral is worth writing to NVS again — and it counts that
    // write as done, so a caller told yes has to make it. The sketch does the
    // writing because the NVS namespace is the sketch's.
    //
    // Both tests have to pass: without the distance one a settled house would
    // write forever, and without the clock one a working one would write on every
    // pass. Out of charge nothing is written at all, since the integral is then
    // only mirroring the setpoint that is in charge, which is not worth keeping.
    bool integralDueToSave(uint32_t now) {
        if (!inCharge || now - lastIntegralSave < integralSaveWait ||
            fabs(control.integral - savedIntegral) < piIntegralSaveEpsilon) {
            return false;
        }
        savedIntegral = control.integral;
        lastIntegralSave = now;
        jitterIntegralSave();
        return true;
    }

    // The setting that goes by this name, or null — which is also how /set tells
    // a parameter of this source from one nothing owns.
    const PISetting* setting(const String& name) const {
        for (int i = 0; i < piSettingCount; i++) {
            if (name == piSettings[i].name) {
                return &piSettings[i];
            }
        }
        return NULL;
    }

    float value(const PISetting& setting) const {
        return control.*(setting.member);
    }

    // /set saves what it sets, so an integral that arrives this way is in NVS
    // already: count it as saved, or the wait would write the same number again.
    void set(const PISetting& setting, float value) {
        control.*(setting.member) = value;
        if (setting.member == &PIController::integral) {
            savedIntegral = value;
        }
    }

    // Which state the controller is in, which is the whole of what this source
    // has to decide: it is in charge only when the switch above says so, and the
    // boiler can only answer the error while it is allowed to heat. `driven` is
    // the setpoint in charge instead, and goes unread unless this source is out.
    void update(bool nowInCharge, bool canHeat, float measured, float driven, uint32_t now) {
        inCharge = nowInCharge;
        if (!inCharge) {
            control.track(measured, driven, now);
        } else if (canHeat) {
            control.drive(measured, now);
        } else {
            control.hold(measured, now);
        }
    }

    // The two below are templated only so this header needs neither ArduinoJson
    // nor Preferences: whichever document and NVS types the sketch is built with
    // are the ones they get.
    template <typename Doc>
    void report(Doc& doc) const {
        for (int i = 0; i < piSettingCount; i++) {
            doc[piGroup][piSettings[i].name + piNameOffset] = value(piSettings[i]);
        }
        doc[piGroup]["output"] = control.output;
    }

    template <typename Prefs>
    void load(Prefs& preferences) {
        for (int i = 0; i < piSettingCount; i++) {
            set(piSettings[i], preferences.getFloat(piSettings[i].name, value(piSettings[i])));
        }
        savedIntegral = control.integral;  // restored, so there is nothing to write back
    }
};
