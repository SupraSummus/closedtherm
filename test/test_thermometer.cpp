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

}  // namespace

TEST_CASE("a first pass is reported as it is, at 2 mV per degree, downwards as it warms") {
    Thermometer t(pin);
    passes(t, 1, 677);
    CHECK(t.millivolts == 677);  // seeds the low-pass rather than easing up from zero
    CHECK(t.celsius == 15);      // 6 mV above the 671 mV of 18 C
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

TEST_CASE("the low-pass follows a step of the median with a time constant of about a minute") {
    Thermometer t(pin);
    passes(t, Thermometer::window, 700);
    passes(t, Thermometer::window, 600);  // the median is at 600 within the window...
    CHECK(t.median == 600);
    CHECK(t.millivolts > 690);  // ...the low-pass has barely started
    passes(t, 600 - Thermometer::window, 600);  // a minute since the step, at 100 ms a pass
    CHECK(t.millivolts == doctest::Approx(700 - 100 * 0.632).epsilon(0.01));  // 1 - 1/e of the way
}

TEST_CASE("the low-pass takes millis() wrapping past 32 bits as the second it was") {
    Thermometer t(pin);
    fake::millis = UINT32_MAX - 1000;
    passes(t, 1, 700, 0);  // seeded a second before the wrap
    fake::millis = 0;
    passes(t, 1, 600, 0);  // the median is 650 now, one second on through zero
    CHECK(t.millivolts < 700);
    CHECK(t.millivolts > 690);  // a second of the way, not 49 days of it
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
