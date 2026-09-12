// The controller on its own: no sketch, no sensor, no boiler, and no clock but
// the one each test hands it. What it does with the number it is given lives
// here; which state ardu.ino puts it in is tested in test_sketch.cpp.
#include "doctest.h"
#include "pi.h"

namespace {

// A controller aiming at 21, with the proportional term switched off so the
// tests read the integral straight off the output, and a gain fast enough that
// they do not have to simulate an hour.
PIController integralOnly() {
    PIController pi;
    pi.target = 21;
    pi.kp = 0;
    pi.ki = 1.0;  // a degree of output per second per degree of error
    pi.start(0);
    return pi;
}

}  // namespace

TEST_CASE("the integral follows elapsed time, not the number of calls") {
    PIController often = integralOnly();
    PIController seldom = integralOnly();
    for (unsigned long t = 1000; t <= 10000; t += 1000) {
        often.drive(18, t);
    }
    seldom.drive(18, 10000);
    CHECK(often.integral == doctest::Approx(seldom.integral));
    CHECK(often.integral == doctest::Approx(often.outMin + 30));  // 10 s of a 3 degree error
}

TEST_CASE("the integral does not wind up past the output range") {
    PIController pi = integralOnly();
    for (unsigned long t = 1000; t <= 60000; t += 1000) {
        pi.drive(18, t);  // far longer than it takes to reach the ceiling
    }
    CHECK(pi.integral == pi.outMax);

    pi.drive(24, 61000);  // the reading overshoots
    CHECK(pi.integral == doctest::Approx(pi.outMax - 3));  // backs off at once, nothing banked
}

TEST_CASE("the output follows the error and stays inside the range") {
    PIController pi;
    pi.target = 21;
    pi.ki = 0;  // the proportional term alone, so the numbers are the gain
    pi.start(0);

    pi.drive(18, 1000);
    CHECK(pi.error == doctest::Approx(3));
    CHECK(pi.output == doctest::Approx(pi.outMin + pi.kp * 3));

    pi.drive(24, 2000);
    CHECK(pi.output == pi.outMin);  // asks for as little as it can, not for less

    pi.kp = 100;
    pi.drive(0, 3000);
    CHECK(pi.output == pi.outMax);
}

TEST_CASE("holding leaves the integral alone, and does not bank the time either") {
    PIController pi = integralOnly();
    for (unsigned long t = 1000; t <= 20000; t += 1000) {
        pi.hold(18, t);
    }
    CHECK(pi.integral == pi.outMin);

    pi.drive(18, 21000);
    CHECK(pi.integral == doctest::Approx(pi.outMin + 3));  // one second, not the twenty held
}

TEST_CASE("tracking sits where the output matches what is in charge") {
    PIController pi;
    pi.target = 21;
    pi.ki = 1.0;  // would run away in seconds if tracking integrated
    pi.start(0);
    for (unsigned long t = 1000; t <= 20000; t += 1000) {
        pi.track(18, 55, t);
    }
    CHECK(pi.output == doctest::Approx(55));
    CHECK(pi.integral == doctest::Approx(55 - pi.kp * 3));  // held wherever that takes

    pi.drive(18, 21000);
    CHECK(pi.output == doctest::Approx(55 + 3));  // takes over from there, one second on
}

TEST_CASE("tracking cannot follow past the output range") {
    PIController pi;
    pi.target = 21;
    pi.start(0);
    pi.track(21, 95, 1000);  // no error, so the output is the integral
    CHECK(pi.output == pi.outMax);      // matches it as far as it reaches
}
