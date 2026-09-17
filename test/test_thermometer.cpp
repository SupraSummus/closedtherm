// thermometer.h on its own, against the fake ADC: a test says what the ADC
// reads and checks what the thermometer makes of it.
#include <initializer_list>

#include "doctest.h"
#include "../thermometer.h"

namespace {

const int pin = 35;

// So many passes over a steady sensor, this far apart on the clock.
void passes(Thermometer& t, int count, int mv, uint32_t stepMs = 100) {
    fake::adc_mv = mv;
    for (int i = 0; i < count; i++) {
        fake::millis += stepMs;
        t.sample();
    }
}

// One pass per reading, in this order; the median they come to.
float medianOver(std::initializer_list<int> readings) {
    fake::adc_readings.assign(readings.begin(), readings.end());
    Thermometer t(pin);
    for (size_t i = 0; i < readings.size(); i++) {
        t.sample();
    }
    return t.median;
}

// A full window at 700 mV, then ten minutes in one pass, which is all the
// warm-up any constant here asks for, then the median stepped to 600 with no
// time gone by: a 100 mV step the low-pass has not answered yet.
//
// One thermometer's passes must not be interleaved with another's, since
// `passes` moves the one clock they all read: a gap reaches the one left waiting
// as a pass ten minutes long. Hence a helper per profile, taking a constant and
// handing back a reading, rather than a test driving two thermometers at once.
void aStepAfterWarmUp(Thermometer& t) {
    passes(t, Thermometer::window, 700);
    passes(t, 1, 700, 600000);
    passes(t, Thermometer::window, 600, 0);
}

// How much of the 100 mV step the low-pass has followed a minute after it.
float aMinutePastAStep(float timeConstant) {
    Thermometer t(pin);
    t.setTimeConstant(timeConstant);
    aStepAfterWarmUp(t);
    passes(t, 600, 600);  // a minute of the step, at 100 ms a pass
    return 700 - t.millivolts;
}

// Where the low-pass reads a second after the constant was changed at the step.
float aSecondAfterChanging(float before, float after) {
    Thermometer t(pin);
    t.setTimeConstant(before);
    aStepAfterWarmUp(t);
    t.setTimeConstant(after);
    passes(t, 1, 600, 1000);
    return t.millivolts;
}

}  // namespace

TEST_CASE("a first pass is reported as it is, at 2 mV per degree, downwards as it warms") {
    Thermometer t(pin);
    passes(t, 1, 677);
    CHECK(t.millivolts == 677);  // seeds the low-pass rather than easing up from zero
    CHECK(t.celsius == 15);      // 6 mV above the 671 mV of 18 C
}

TEST_CASE("a first pass that reads off counts for one pass, not for ten minutes") {
    Thermometer t(pin);
    passes(t, 1, 696);  // 25 mV high, which is 12 degrees cold: what a boot has been seen to read
    passes(t, 9, 671);
    // The mean of the ten medians: 696, 683.5 while it is one reading of two, then 671.
    // A ten-minute low-pass seeded with it would still be within a millivolt of it.
    CHECK(t.millivolts == doctest::Approx(674.75));
    CHECK(t.celsius == doctest::Approx(16.125));  // two degrees off, not twelve
}

TEST_CASE("the median is over the readings taken so far, until the window fills") {
    CHECK(medianOver({701}) == 701);            // the one reading, not it over a full window
    CHECK(medianOver({701, 601}) == 651);       // an even count: the mean of the middle two
    CHECK(medianOver({701, 600}) == 650.5);     // ...kept to the half millivolt, not rounded
    CHECK(medianOver({701, 601, 611}) == 611);  // an odd count: the middle one
}

TEST_CASE("a half millivolt converts to a quarter degree") {
    CHECK(Thermometer::toCelsius(650.5) == 28.25);  // 20.5 mV below the 671 mV of 18 C
}

TEST_CASE("the median drops dips that an average would follow") {
    // Two passes out of five land on a supply dip and read 30 mV low. An
    // average would come out 12 mV low, which is 6 degrees warm.
    CHECK(medianOver({671, 641, 671, 641, 671}) == 671);
}

TEST_CASE("a reading counts for as long as the window holds it, and no longer") {
    Thermometer t(pin);
    passes(t, Thermometer::window, 700);      // a full window of 700...
    passes(t, Thermometer::window / 2, 600);  // ...half of it since overwritten with 600
    CHECK(t.median == 650);                   // split down the middle
    passes(t, 1, 600);
    CHECK(t.median == 600);  // the 600s are the majority now
}

TEST_CASE("the low-pass follows a step of the median with a time constant of about ten minutes") {
    Thermometer t(pin);
    passes(t, Thermometer::window, 700);
    passes(t, 1, 700, 600000);            // ten minutes in: the time constant is at full length
    passes(t, Thermometer::window, 600);  // the median is at 600 within the window...
    CHECK(t.median == 600);
    CHECK(t.millivolts > 699);  // ...the low-pass has barely started
    passes(t, 6000 - Thermometer::window, 600);  // ten minutes since the step, at 100 ms a pass
    CHECK(t.millivolts == doctest::Approx(700 - 100 * 0.632).epsilon(0.01));  // 1 - 1/e of the way
}

TEST_CASE("the time constant is a setting, and a shorter one follows a step sooner") {
    // A minute of a minute's constant is one constant: 1 - 1/e of the step. A
    // minute of ten minutes' is a tenth of one, and barely a tenth of the way.
    CHECK(aMinutePastAStep(60) == doctest::Approx(100 * 0.632).epsilon(0.01));
    CHECK(aMinutePastAStep(600) == doctest::Approx(100 * 0.095).epsilon(0.01));
}

TEST_CASE("a time constant of zero is the median, unfiltered") {
    Thermometer t(pin);
    t.setTimeConstant(0);
    aStepAfterWarmUp(t);
    CHECK(t.millivolts == 700);  // no time has gone by, so not even this one has moved
    passes(t, 1, 600);           // and one pass with any time in it goes all the way
    CHECK(t.millivolts == 600);
}

TEST_CASE("a constant changed mid-run is in force from the next pass") {
    // The warm-up is how long sampling has been going, which is a fact about the
    // past, so nothing has to catch up with a new setting: a second of a
    // minute's constant is 1/61 of the step and a second of ten minutes' is
    // 1/601, whichever constant the thermometer had been running with before.
    CHECK(aSecondAfterChanging(600, 60) == doctest::Approx(700 - 100.0 / 61).epsilon(0.001));
    CHECK(aSecondAfterChanging(60, 600) == doctest::Approx(700 - 100.0 / 601).epsilon(0.001));
}

TEST_CASE("the low-pass takes millis() wrapping past 32 bits as the second it was") {
    Thermometer t(pin);
    fake::millis = UINT32_MAX - 1000;
    passes(t, 1, 700, 0);  // seeded a second before the wrap
    fake::millis = 0;
    passes(t, 1, 600, 0);  // the median is 650 now, one second on through zero
    // A second in, the mean of the two medians; 49 days would have taken it nearly to 650.
    CHECK(t.millivolts == doctest::Approx(675).epsilon(0.001));
}

TEST_CASE("the low-pass runs on the clock, not on passes") {
    Thermometer t(pin);
    passes(t, Thermometer::window, 700);
    passes(t, 1000, 600, 0);  // a thousand passes, no time gone by
    CHECK(t.median == 600);
    CHECK(t.millivolts == 700);  // not moved
    passes(t, 1, 600, 60000);  // one pass, a minute later
    CHECK(t.millivolts < 700);
    CHECK(t.millivolts > 600);  // moved, and not all the way however long the gap
}
