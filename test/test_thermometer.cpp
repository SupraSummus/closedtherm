// thermometer.h on its own, against the fake ADC: a test says what the ADC
// reads and checks what the thermometer makes of it.
#include <initializer_list>

#include "doctest.h"
#include "../thermometer.h"

namespace {

const int pin = 35;

// So many passes over a steady sensor.
void passes(Thermometer& t, int count, int mv) {
    fake::adc_mv = mv;
    for (int i = 0; i < count; i++) {
        t.sample();
    }
}

// One pass per reading, in this order.
float passesOver(std::initializer_list<int> readings) {
    fake::adc_readings.assign(readings.begin(), readings.end());
    Thermometer t(pin);
    for (size_t i = 0; i < readings.size(); i++) {
        t.sample();
    }
    return t.millivolts;
}

}  // namespace

TEST_CASE("millivolts convert at 2 mV per degree, downwards as it warms") {
    Thermometer t(pin);
    passes(t, 1, 677);
    CHECK(t.millivolts == 677);
    CHECK(t.celsius == 15);  // 6 mV above the 671 mV of 18 C
}

TEST_CASE("the median is over the readings taken so far, until the window fills") {
    CHECK(passesOver({701}) == 701);            // the one reading, not it over a full window
    CHECK(passesOver({701, 601}) == 651);       // an even count: the mean of the middle two
    CHECK(passesOver({701, 601, 611}) == 611);  // an odd count: the middle one
}

TEST_CASE("the median drops dips that an average would follow") {
    // Two passes out of five land on a supply dip and read 30 mV low. An
    // average would come out 12 mV low, which is 6 degrees warm.
    CHECK(passesOver({671, 641, 671, 641, 671}) == 671);
}

TEST_CASE("a reading counts for as long as the window holds it, and no longer") {
    Thermometer t(pin);
    passes(t, Thermometer::window, 700);      // a full window of 700...
    passes(t, Thermometer::window / 2, 600);  // ...half of it since overwritten with 600
    CHECK(t.millivolts == 650);               // split down the middle
    passes(t, 1, 600);
    CHECK(t.millivolts == 600);  // the 600s are the majority now
}
