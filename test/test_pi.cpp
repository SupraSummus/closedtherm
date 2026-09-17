// The controller on its own: no sketch, no sensor, no boiler, and no clock but
// the one each test hands it. What it does with the number it is given lives
// here; which state ardu.ino puts it in is tested in test_sketch.cpp.
#include "doctest.h"
#include "pi.h"

namespace {

// A controller aiming at 21, with the proportional term switched off so the
// tests read the integral straight off the output, and a gain fast enough that
// they do not have to simulate an hour. A gain of zero is fine here and not over
// HTTP: the controller is arithmetic either way, but pi_source.h reads the sign of
// the error out of the output, which at zero it does not carry. Every test below
// reads a room of 18, and
// the integral starts there because that is the floor such a room puts under it:
// starting anywhere lower would only measure the first call lifting it.
const float room = 18;

PIController integralOnly() {
    PIController pi;
    pi.target = 21;
    pi.kp = 0;
    pi.ki = 1.0;  // a degree of output per second per degree of error
    pi.integral = room;
    pi.start(0);
    return pi;
}

}  // namespace

TEST_CASE("the integral follows elapsed time, not the number of calls") {
    PIController often = integralOnly();
    PIController seldom = integralOnly();
    for (uint32_t t = 1000; t <= 10000; t += 1000) {
        often.drive(18, t);
    }
    seldom.drive(18, 10000);
    CHECK(often.integral == doctest::Approx(seldom.integral));
    CHECK(often.integral == doctest::Approx(room + 30));  // 10 s of a 3 degree error
}

TEST_CASE("millis() wrapping past 32 bits is one more second, not 49 days back") {
    PIController pi = integralOnly();
    pi.start(UINT32_MAX - 500);
    pi.drive(18, 500);  // 1001 ms later, having gone through zero
    CHECK(pi.integral == doctest::Approx(room + 3).epsilon(0.01));
}

TEST_CASE("the integral does not wind up past the output range") {
    PIController pi = integralOnly();
    for (uint32_t t = 1000; t <= 60000; t += 1000) {
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
    CHECK(pi.output == doctest::Approx(18 + pi.kp * 3));  // off the floor the room puts down

    pi.drive(24, 2000);
    CHECK(pi.output == pi.outMin);  // asks for as little as it can, not for less

    pi.kp = 100;
    pi.drive(0, 3000);
    CHECK(pi.output == pi.outMax);
}

// A caller has to be able to ask what a reading gives before committing to a
// state, so the answer has to hold once a state that does not integrate has run —
// stale integral and all, which is the case the floor decides.
TEST_CASE("outputFor answers what holding would leave, from an integral under the room") {
    PIController pi;
    pi.target = 21;
    pi.integral = pi.outMin;
    pi.start(0);

    float predicted = pi.outputFor(20);
    pi.hold(20, 1000);
    CHECK(pi.output == doctest::Approx(predicted));
    CHECK(predicted > 20);  // where outMin + kp * 1 would have come out under the room
}

// An integral under the reading is a base the controller cannot act from, and one
// no state may leave behind: at zero error the output would come out under the
// reading, and a caller that reads that as a reason to stop calling drive() would
// have nothing left that could raise it.
TEST_CASE("the integral is never left under the reading, whichever state ran") {
    PIController pi;
    pi.target = 21;
    pi.ki = 1.0;
    pi.start(0);

    pi.hold(19, 1000);
    CHECK(pi.integral == 19);

    pi.integral = pi.outMin;
    pi.drive(19, 2000);
    CHECK(pi.integral >= 19);

    pi.integral = pi.outMin;
    pi.track(19, 8, 3000);  // something else asking for less than the room
    CHECK(pi.integral == 19);
}

TEST_CASE("holding leaves the integral alone, and does not bank the time either") {
    PIController pi = integralOnly();
    for (uint32_t t = 1000; t <= 20000; t += 1000) {
        pi.hold(18, t);
    }
    CHECK(pi.integral == room);

    pi.drive(18, 21000);
    CHECK(pi.integral == doctest::Approx(room + 3));  // one second, not the twenty held
}

TEST_CASE("tracking sits where the output matches what is in charge") {
    PIController pi;
    pi.target = 21;
    pi.ki = 1.0;  // would run away in seconds if tracking integrated
    pi.start(0);
    for (uint32_t t = 1000; t <= 20000; t += 1000) {
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
