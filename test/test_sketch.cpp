// Compiles the real ardu.ino against test/fakes/ and drives it the way the
// ESP32 would: setup(), loop() with a controlled clock, HTTP handlers through
// the recorded routes. Only this file may include the sketch.
#include <initializer_list>

#include "doctest.h"
#include "../ardu.ino"

namespace {

// Cold boot with empty NVS. Globals persist between test cases, so reset the
// ones setup() and loop() carry over — the four settings included: setup()
// reads each from NVS with its current value as the default, so one a previous
// test switched off would otherwise stay off, and the suite would pass or fail
// by the order it happened to run in.
struct Booted {
    Booted() {
        preferences = Preferences();
        setCentralHeatingOn = setHotWaterOn = true;
        setBoilerTemperature = 60;
        setDHWTemperature = 55;
        boot_count = 0;
        wifi_reconnects = 0;
        lastWifiConnected = 0;
        thermometer = Thermometer(tempSensorPin);
        fake::adc_mv = 671;
        fake::adc_readings.clear();
        fake::millis = 0;  // setup() starts the clocks from here, as a boot does
        fake::random_value = piIntegralSaveJitter;  // jitter of exactly none, for round numbers
        lastSetpointSent = 0;
        sentBoilerTemperature = sentDHWTemperature = NAN;
        ot.ch_setpoints = ot.dhw_setpoints = 0;
        server.pending = nullptr;
        server.pending_in = 0;
        pi = PISource();
        setChTempSource = CH_TEMP_MANUAL;
        setup();
        WiFi.begins = WiFi.disconnects = 0;  // setup() connected once; count kicks from here
    }
};

// One loop() iteration with the link in the given state at the given time.
void tick(bool online, uint32_t ms) {
    WiFi.status_ = online ? WL_CONNECTED : WL_DISCONNECTED;
    fake::millis = ms;
    loop();
}

// Points the sensor at a room temperature, in the millivolts loop() converts back,
// and clears the window so the next tick reports it outright instead of the
// median of it and the readings before it.
void roomTemperature(float celsius) {
    fake::adc_mv = static_cast<int>(Thermometer::toMillivolts(celsius));
    thermometer = Thermometer(tempSensorPin);
}

JsonDocument parsed(const WebServer::Response& response) {
    JsonDocument doc;
    REQUIRE(deserializeJson(doc, response.body) == DeserializationError::Ok);
    return doc;
}

JsonDocument status() { return parsed(server.get("/")); }

}  // namespace

TEST_CASE("setpoints and boot count survive a reboot") {
    preferences = Preferences();
    preferences.floats["req_ch_temp"] = 41;
    preferences.floats["req_dhw_temp"] = 42;
    preferences.bools["ch_on"] = false;
    preferences.bools["dhw_on"] = false;
    preferences.uints["boot_count"] = 7;
    setup();
    CHECK(setBoilerTemperature == 41);
    CHECK(setDHWTemperature == 42);
    CHECK_FALSE(setCentralHeatingOn);
    CHECK_FALSE(setHotWaterOn);
    CHECK(boot_count == 8);
    CHECK(preferences.uints["boot_count"] == 8);
}

TEST_CASE("/set saves a setpoint under the key setup() reads and answers with the new state") {
    Booted b;
    WebServer::Response response = server.get("/set", {{"requested_ch_temp", "45"}});
    CHECK(response.code == 200);
    CHECK(setBoilerTemperature == 45);
    CHECK(preferences.floats.at("req_ch_temp") == 45);
    CHECK(parsed(response)["requested_ch_temp"].as<float>() == 45);  // same key /set takes and / reports
}

TEST_CASE("/set takes several settings at once, switches both ways") {
    Booted b;
    CHECK(server.get("/set", {{"requested_ch_on", "off"},
                              {"requested_dhw_on", "off"},
                              {"requested_ch_temp", "48"},
                              {"requested_dhw_temp", "52"}}).code == 200);
    CHECK_FALSE(setCentralHeatingOn);
    CHECK_FALSE(setHotWaterOn);
    CHECK(setBoilerTemperature == 48);
    CHECK(setDHWTemperature == 52);
    CHECK_FALSE(preferences.bools.at("ch_on"));
    CHECK_FALSE(preferences.bools.at("dhw_on"));
    CHECK(preferences.floats.at("req_ch_temp") == 48);
    CHECK(preferences.floats.at("req_dhw_temp") == 52);

    CHECK(server.get("/set", {{"requested_ch_on", "on"}, {"requested_dhw_on", "on"}}).code == 200);
    CHECK(setCentralHeatingOn);
    CHECK(setHotWaterOn);
    CHECK(preferences.bools.at("ch_on"));
    CHECK(preferences.bools.at("dhw_on"));
}

TEST_CASE("loop() asks the boiler for the switch settings it was given") {
    Booted b;
    server.get("/set", {{"requested_ch_on", "on"}, {"requested_dhw_on", "off"}});
    tick(true, 1000);
    CHECK(ot.asked_central_heating);
    CHECK_FALSE(ot.asked_hot_water);

    server.get("/set", {{"requested_ch_on", "off"}, {"requested_dhw_on", "on"}});
    tick(true, 2000);
    CHECK_FALSE(ot.asked_central_heating);
    CHECK(ot.asked_hot_water);
}

TEST_CASE("/set rejects a bad value and changes nothing") {
    Booted b;
    setCentralHeatingOn = true;
    setBoilerTemperature = 60;
    setDHWTemperature = 55;
    for (const char* bad : {"0", "100", "-5", "500", "nan", "inf", "abc", ""}) {
        CAPTURE(bad);
        CHECK(server.get("/set", {{"requested_ch_temp", bad}}).code == 400);
    }
    CHECK(server.get("/set", {{"requested_ch_on", "maybe"}}).code == 400);
    // A good parameter alongside a bad one is not applied either.
    CHECK(server.get("/set", {{"requested_ch_on", "off"},
                              {"requested_ch_temp", "48"},
                              {"requested_dhw_temp", "999"}}).code == 400);
    CHECK(setCentralHeatingOn);
    CHECK(setBoilerTemperature == 60);
    CHECK(setDHWTemperature == 55);
    CHECK(preferences.floats.empty());
    CHECK(preferences.bools.empty());
}

TEST_CASE("/set rejects a request it cannot act on instead of silently doing nothing") {
    Booted b;
    setBoilerTemperature = 60;
    CHECK(server.get("/set").code == 400);                             // no parameters
    CHECK(server.get("/set", {{"ch_temp", "45"}}).code == 400);        // reading's name, not the setpoint's
    CHECK(server.get("/set", {{"requested_dwh_temp", "45"}}).code == 400);  // typo
    CHECK(setBoilerTemperature == 60);
    CHECK(preferences.floats.empty());
}

TEST_CASE("loop() forces a WiFi reconnect after 30 s offline, and again every 30 s") {
    Booted b;
    tick(true, 1000);    // online: nothing to do
    tick(false, 20000);  // just dropped: let the driver try first
    CHECK(WiFi.begins == 0);
    tick(false, 31001);  // down >30 s: kick
    CHECK(WiFi.disconnects == 1);
    CHECK(WiFi.begins == 1);
    tick(false, 40000);  // <30 s since the kick
    CHECK(WiFi.begins == 1);
    tick(false, 61002);  // still down: kick again
    tick(false, 91003);  // and again
    CHECK(WiFi.begins == 3);
    CHECK(status()["wifi_reconnects"].as<int>() == 3);
}

TEST_CASE("coming back online restarts the reconnect countdown and keeps the count") {
    Booted b;
    tick(false, 30001);
    CHECK(WiFi.begins == 1);
    tick(true, 35000);   // back online
    tick(false, 60000);  // dropped again at 35 s: not yet
    CHECK(WiFi.begins == 1);
    tick(false, 65001);
    CHECK(WiFi.begins == 2);
    CHECK(wifi_reconnects == 2);
}

// The arithmetic is test_thermometer.cpp's; this is the wiring.
TEST_CASE("loop() takes one thermometer pass off the ADC and / reports it") {
    Booted b;
    fake::adc_mv = 701;
    tick(true, 1000);
    JsonDocument doc = status();
    CHECK(doc["temp_sensor_mv"].as<float>() == 701);
    CHECK(doc["temp_sensor_c"].as<float>() == 3);
    fake::adc_mv = 601;
    tick(true, 2000);
    CHECK(status()["temp_sensor_mv"].as<float>() == 651);  // the median of the two passes
}

TEST_CASE("pushSetpoints() writes to the boiler every 10 s, not on every loop() pass") {
    Booted b;
    setBoilerTemperature = 60;
    setDHWTemperature = 55;

    tick(true, 1000);  // first pass after boot: push what we have
    CHECK(ot.ch_setpoints == 1);
    CHECK(ot.dhw_setpoints == 1);
    CHECK(ot.last_ch_setpoint == 60);
    CHECK(ot.last_dhw_setpoint == 55);

    tick(true, 5000);   // <10 s since the write: nothing to resend
    tick(true, 10999);
    CHECK(ot.ch_setpoints == 1);

    tick(true, 11001);  // 10 s elapsed: refresh the boiler
    CHECK(ot.ch_setpoints == 2);
    CHECK(ot.dhw_setpoints == 2);

    // A new setpoint does not wait for the interval to run out.
    CHECK(server.get("/set", {{"requested_ch_temp", "47"}}).code == 200);
    tick(true, 12000);
    CHECK(ot.ch_setpoints == 3);
    CHECK(ot.last_ch_setpoint == 47);
    tick(true, 13000);  // and the interval restarts from that write
    CHECK(ot.ch_setpoints == 3);
}

TEST_CASE("a setpoint change smaller than the deadband waits for the next refresh") {
    Booted b;
    setBoilerTemperature = 60;
    tick(true, 1000);
    CHECK(ot.ch_setpoints == 1);

    CHECK(server.get("/set", {{"requested_ch_temp", "60.2"}}).code == 200);
    tick(true, 2000);
    CHECK(ot.ch_setpoints == 1);  // too small a move to be worth an exchange of its own
    CHECK(ot.last_ch_setpoint == 60);

    tick(true, 11001);  // ... but the 10 s refresh carries it anyway
    CHECK(ot.ch_setpoints == 2);
    CHECK(ot.last_ch_setpoint == doctest::Approx(60.2));
}

TEST_CASE("a PI output drifting inside the deadband rides the 10 s refresh, not a write per pass") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    pi.control.kp = 0;
    pi.control.ki = 0.01;  // 0.03 C per second at 3 below target: moves every pass, well inside 0.5
    roomTemperature(18);
    for (uint32_t t = 1000; t <= 10000; t += 1000) {
        tick(true, t);
    }
    CHECK(pi.control.output > pi.control.outMin + 0.2);  // it did move...
    CHECK(ot.ch_setpoints == 1);                          // ...but not enough to be worth an exchange each
    tick(true, 11001);
    CHECK(ot.ch_setpoints == 2);
    CHECK(ot.last_ch_setpoint == doctest::Approx(pi.control.output));  // the refresh carries the drift
}

// loop() lets the server in between OpenTherm exchanges, so a /set can land
// after one setpoint has gone out and before the other has. Neither may be lost
// to the throttle, whichever side of its exchange it lands on.
TEST_CASE("a /set served between the two setpoint exchanges reaches the boiler by the next pass") {
    Booted b;
    setBoilerTemperature = 60;
    setDHWTemperature = 55;
    int ch_when_served = -1, dhw_when_served = -1;
    // The fourth handleClient() of a pass is the one between the CH write and the
    // DHW write. That count is checked at serve time rather than trusted, so a
    // reordered loop() fails here instead of testing something else.
    server.pending_in = 4;
    server.pending = [&] {
        ch_when_served = ot.ch_setpoints;
        dhw_when_served = ot.dhw_setpoints;
        CHECK(server.get("/set", {{"requested_ch_temp", "47"}, {"requested_dhw_temp", "50"}}).code == 200);
    };
    tick(true, 1000);
    CHECK(ch_when_served == 1);
    CHECK(dhw_when_served == 0);
    CHECK_FALSE(server.pending);  // it was served

    tick(true, 2000);  // <10 s on, so only the change detection can carry them
    CHECK(ot.last_ch_setpoint == 47);
    CHECK(ot.last_dhw_setpoint == 50);
}

// How the controller itself behaves is in test_pi.cpp. These cover the wiring:
// the state the sketch puts it in, and the reading and switch it really uses.
TEST_CASE("ch_temp_source decides which setpoint reaches the boiler") {
    Booted b;
    setBoilerTemperature = 60;
    pi.control.ki = 0;    // so the controller holds still and only the routing moves
    roomTemperature(18);
    tick(true, 1000);
    CHECK(ot.last_ch_setpoint == 60);
    CHECK(status()["ch_temp_source"].as<std::string>() == "manual");

    // With the controller in charge, moving the manual setpoint moves nothing.
    CHECK(server.get("/set", {{"ch_temp_source", "pi"},
                              {"requested_ch_temp", "70"}}).code == 200);
    tick(true, 2000);
    CHECK(ot.last_ch_setpoint == 60);  // the boiler is never told 70
    CHECK(status()["ch_temp_source"].as<std::string>() == "pi");
    CHECK(status()["effective_ch_temp"].as<float>() == doctest::Approx(60));
    CHECK(status()["requested_ch_temp"].as<float>() == 70);  // kept, just not in charge

    CHECK(server.get("/set", {{"ch_temp_source", "manual"}}).code == 200);
    tick(true, 3000);
    CHECK(ot.last_ch_setpoint == 70);
}

TEST_CASE("out of charge the controller tracks, so the switch does not jump the boiler") {
    Booted b;
    setBoilerTemperature = 55;
    pi.control.ki = 1.0;  // would run away in seconds if the sketch let it integrate
    roomTemperature(18);  // 3 below target, so there is an error to run away on
    for (uint32_t t = 1000; t <= 20000; t += 1000) {
        tick(true, t);
    }
    CHECK(pi.control.output == doctest::Approx(55));  // sitting on what the boiler is being sent

    CHECK(server.get("/set", {{"ch_temp_source", "pi"}}).code == 200);
    tick(true, 21000);
    CHECK(ot.last_ch_setpoint == doctest::Approx(55 + 3));  // takes over from there, 1 s on
}

TEST_CASE("the controller holds while central heating is switched off") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    pi.control.kp = 0;  // integral alone, so the output reads it back directly
    pi.control.ki = 1.0;
    roomTemperature(18);
    CHECK(server.get("/set", {{"requested_ch_on", "off"}}).code == 200);
    for (uint32_t t = 1000; t <= 20000; t += 1000) {
        tick(true, t);
    }
    CHECK(pi.control.integral == pi.control.outMin);  // an error the boiler was never asked to answer

    CHECK(server.get("/set", {{"requested_ch_on", "on"}}).code == 200);
    tick(true, 21000);
    CHECK(pi.control.integral == doctest::Approx(pi.control.outMin + 3));  // and it picks up from there
}

TEST_CASE("the controller is given the reading the sketch reports, in degrees") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    roomTemperature(18);  // 3 below the 21 C target
    tick(true, 1000);
    CHECK(status()["temp_sensor_c"].as<float>() == doctest::Approx(18));
    CHECK(pi.control.error == doctest::Approx(3));  // degrees, not the millivolts beside them
}

TEST_CASE("the integral survives a reboot instead of climbing back from the floor") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    pi.control.ki = 0.01;
    roomTemperature(18);  // 3 below target
    tick(true, 601000);   // long enough to learn something, and to be worth a write
    float learned = pi.control.integral;
    CHECK(learned > pi.control.outMin + 1);
    CHECK(preferences.floats.at("pi_integral") == doctest::Approx(learned));

    pi = PISource();  // as a fresh boot has it
    setup();
    CHECK(pi.control.integral == doctest::Approx(learned));
}

TEST_CASE("a write waits for the integral to have moved and for the wait to have passed") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    pi.control.ki = 0.01;
    roomTemperature(18);
    tick(true, 599000);  // moved plenty, but not yet due
    CHECK(preferences.floats.count("pi_integral") == 0);

    tick(true, 601000);
    float written = preferences.floats.at("pi_integral");

    roomTemperature(21);   // on target, so the integral stops moving
    tick(true, 1300000);   // another wait passes with nothing to say
    CHECK(preferences.floats.at("pi_integral") == written);
}

TEST_CASE("an integral seeded over /set counts as saved, so the wait does not write it again") {
    Booted b;
    setChTempSource = CH_TEMP_PI;
    roomTemperature(21);  // on target, so nothing but /set moves the integral
    CHECK(server.get("/set", {{"pi_integral", "40"}}).code == 200);
    CHECK(preferences.floats.at("pi_integral") == 40);  // /set wrote it...
    preferences.floats.erase("pi_integral");            // ...so a second write of it would show up here
    tick(true, 1300000);  // well past the wait
    CHECK(preferences.floats.count("pi_integral") == 0);
}

TEST_CASE("nothing is written while the controller is out of charge") {
    Booted b;  // manual is in charge
    setBoilerTemperature = 70;
    roomTemperature(18);
    tick(true, 601000);
    CHECK(pi.control.integral > pi.control.outMin + 1);  // tracking moved it a long way
    CHECK(preferences.floats.count("pi_integral") == 0);  // but it is not ours to keep
}

TEST_CASE("the wait between writes is jittered, so they do not fall on a grid") {
    Booted b;
    fake::random_value = 0;
    pi.jitterIntegralSave();
    CHECK(pi.integralSaveWait == piIntegralSaveInterval - piIntegralSaveJitter);

    fake::random_value = 2 * piIntegralSaveJitter - 1;
    pi.jitterIntegralSave();
    CHECK(pi.integralSaveWait == piIntegralSaveInterval + piIntegralSaveJitter - 1);
}

// Walks whatever piSettings holds, so a float added there is covered by the row
// alone: /set takes it, saves it under its key, reports it, and setup() restores
// it.
TEST_CASE("every PI setting goes out to NVS and comes back through /set and /") {
    Booted b;
    for (int i = 0; i < piSettingCount; i++) {
        const PISetting& setting = piSettings[i];
        CAPTURE(setting.name);
        float wanted = (setting.min + setting.max) / 2;

        WebServer::Response response = server.get("/set", {{setting.name, std::to_string(wanted)}});
        CHECK(response.code == 200);
        CHECK(pi.value(setting) == doctest::Approx(wanted));
        CHECK(preferences.floats.at(setting.name) == doctest::Approx(wanted));
        CHECK(parsed(response)["pi"][setting.name + piNameOffset].as<float>() ==
              doctest::Approx(wanted));

        pi.set(setting, 999);  // and a reboot picks it back up
        setup();
        CHECK(pi.value(setting) == doctest::Approx(wanted));
    }
}

TEST_CASE("/set saves the setpoint source as its number, and answers with the new state") {
    Booted b;
    WebServer::Response response = server.get("/set", {{"ch_temp_source", "pi"}});
    CHECK(response.code == 200);
    CHECK(setChTempSource == CH_TEMP_PI);
    CHECK(preferences.uints.at("ch_temp_src") == CH_TEMP_PI);  // the number, so the list can grow
    CHECK(parsed(response)["ch_temp_source"].as<std::string>() == "pi");

    setChTempSource = CH_TEMP_MANUAL;
    setup();
    CHECK(setChTempSource == CH_TEMP_PI);
}

TEST_CASE("a stored source this build does not have falls back to manual") {
    Booted b;
    preferences.uints["ch_temp_src"] = chTempSourceCount + 3;  // from a build with more algorithms
    setChTempSource = CH_TEMP_PI;
    setup();
    CHECK(setChTempSource == CH_TEMP_MANUAL);
    CHECK(status()["ch_temp_source"].as<std::string>() == "manual");  // a name, not an index off the end
}

TEST_CASE("/set rejects PI settings it cannot act on and changes nothing") {
    Booted b;
    CHECK(server.get("/set", {{"ch_temp_source", "auto"}}).code == 400);
    CHECK(server.get("/set", {{"ch_temp_source", "1"}}).code == 400);  // the name, not the stored number
    CHECK(server.get("/set", {{"ch_temp_source", ""}}).code == 400);
    // A room target outside the band is a typo, not a request. Both ends are in.
    for (const char* bad : {"4.9", "35.1", "0", "-5", "abc", "nan", "inf", ""}) {
        CAPTURE(bad);
        CHECK(server.get("/set", {{"pi_target_temp", bad}}).code == 400);
    }
    // 0 is a valid gain, so a value that parses as far as the junk is not good enough.
    for (const char* bad : {"-1", "101", "abc", "inf", "8x", ""}) {
        CAPTURE(bad);
        CHECK(server.get("/set", {{"pi_kp", bad}}).code == 400);
        CHECK(server.get("/set", {{"pi_ki", bad}}).code == 400);
    }
    CHECK(setChTempSource == CH_TEMP_MANUAL);
    CHECK(pi.control.target == 21);
    CHECK(pi.control.kp == 8);
    CHECK(preferences.floats.empty());
    CHECK(preferences.bools.empty());

    CHECK(server.get("/set", {{"pi_target_temp", "5"}}).code == 200);   // both ends are in
    CHECK(server.get("/set", {{"pi_target_temp", "35"}}).code == 200);
}

TEST_CASE("/ reports state as JSON") {
    Booted b;
    server.get("/set", {{"requested_ch_temp", "61"}});
    JsonDocument doc = status();
    CHECK(server.sent.type == "application/json");
    CHECK(doc["requested_ch_temp"].as<float>() == 61);
    CHECK(doc["boot_count"].as<int>() == 1);

    // The controller's own numbers sit in a group of their own, under the name
    // every one of its settings is prefixed with.
    CHECK(doc["pi"]["target_temp"].as<float>() == 21);
    CHECK(doc["pi"]["output"].as<float>() == pi.control.output);
    CHECK(doc["pi_kp"].isNull());  // grouped, not also at the top level
}
