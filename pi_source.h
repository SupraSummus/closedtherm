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
    // The proportional gain has to be strictly positive, which is this source's
    // requirement rather than the controller's: heatDemand reads the sign of the
    // error out of the output, and at a gain of zero the output carries none of it.
    // The integral's floor would then pin the output to the room exactly, the
    // demand would never come out on, and holding would keep it there for good.
    // Any positive value avoids that; 0.1 is just the smallest one worth calling a
    // gain, being far below anything usable.
    {"pi_kp", &PIController::kp, 0.1, 100},
    // Zero is a real setting here: no integral, just proportional, which the floor
    // still keeps above the room.
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

// The shortest the demand may stay in one state, which bounds the boiler to one
// cycle per twice this however the crossing is being wandered over — sensor noise,
// a gain that answers the room too hard, or the loop closing faster than the house
// can. A time rather than a band of degrees: a band would bias where the room
// settles, and this only limits how often the answer may change, not what it is.
const uint32_t piDemandDwell = 600000;  // 10 minutes

struct PISource {
    PIController control;

    // Whether this source was the one in charge as of the last update().
    bool inCharge = false;

    // Whether this source wants the boiler heating at all, as of the last
    // update(), which the sketch reads for the CH enable bit. Water arriving
    // colder than the room it is sent to takes heat out of the house rather than
    // putting it in, so an output below the reading is not a small demand but the
    // wrong sign: what it means is that the boiler should be off. What the boiler
    // does with a setpoint it cannot reach — its own minimum, its own cycling —
    // is the boiler's business and not modelled here.
    //
    // Out of charge it goes with the tracked output, so it then says whether what
    // is driving is above the room, not what this source would do instead. There
    // is no answering that second question: tracking is what the integral is
    // doing while something else drives.
    bool heatDemand = false;
    uint32_t lastDemandChange = 0;

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
        // A whole dwell ago, so the first pass after a boot answers the room it
        // actually finds rather than sitting out ten minutes of it.
        lastDemandChange = now - piDemandDwell;
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
    // boiler can only answer the error while it is both allowed to heat and
    // being asked to. `driven` is the setpoint in charge instead, and goes unread
    // unless this source is out.
    //
    // The demand is settled off the reading the controller is about to act on, so
    // the state it goes into and the CH bit the sketch sends agree on the same pass,
    // and it is the settled one — dwell and all — that picks the state, since
    // holding has to mean the boiler really is off.
    //
    // The dwell costs the integral a little overshoot past the crossing, since
    // drive() carries on for as long as the change is held back: ki * error * dwell,
    // which at the gains above is about a degree. The floor is the bound that
    // matters; the crossing was only ever the soft one.
    //
    // Turning the boiler off is what makes the controller's own floor load-bearing
    // rather than tidy: off is hold(), which does not integrate, so an integral
    // under the room would be one the controller could never raise — it would ask
    // for less than the room, be switched off for it, and stay there. With the
    // floor in pi.h, a room at or below target always comes out asking for heat,
    // whatever the integral was.
    void update(bool nowInCharge, bool allowed, float measured, float driven, uint32_t now) {
        inCharge = nowInCharge;
        bool wanted = control.outputFor(measured) > measured;
        if (wanted != heatDemand && now - lastDemandChange >= piDemandDwell) {
            heatDemand = wanted;
            lastDemandChange = now;
        }
        if (!inCharge) {
            control.track(measured, driven, now);
        } else if (allowed && heatDemand) {
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
        doc[piGroup]["heat_demand"] = heatDemand;
    }

    template <typename Prefs>
    void load(Prefs& preferences) {
        for (int i = 0; i < piSettingCount; i++) {
            set(piSettings[i], preferences.getFloat(piSettings[i].name, value(piSettings[i])));
        }
        savedIntegral = control.integral;  // restored, so there is nothing to write back
    }
};
