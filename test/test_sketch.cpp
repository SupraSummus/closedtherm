// Compiles the real ardu.ino against test/fakes/ and drives it the way the
// ESP32 would: setup(), loop() with a controlled clock, HTTP handlers through
// the recorded routes. Only this file may include the sketch.
#include <initializer_list>

#include "doctest.h"
#include "../ardu.ino"

namespace {

// Cold boot with empty NVS. Globals persist between test cases, so reset the
// ones setup() and loop() carry over.
struct Booted {
    Booted() {
        preferences = Preferences();
        boot_count = 0;
        wifi_reconnects = 0;
        lastWifiConnected = 0;
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

JsonDocument status() {
    JsonDocument doc;
    REQUIRE(deserializeJson(doc, server.get("/").body) == DeserializationError::Ok);
    return doc;
}

struct Endpoint { const char* path; const char* nvs_key; float* setpoint; };
const Endpoint temperature_endpoints[] = {
    {"/set_boiler_temperature", "req_ch_temp", &setBoilerTemperature},
    {"/set_dhw_temperature", "req_dhw_temp", &setDHWTemperature},
};

}  // namespace

TEST_CASE("setpoints and boot count survive a reboot") {
    preferences = Preferences();
    preferences.floats["req_ch_temp"] = 41;
    preferences.floats["req_dhw_temp"] = 42;
    preferences.bools["ch_on"] = false;
    preferences.uints["boot_count"] = 7;
    setup();
    CHECK(setBoilerTemperature == 41);
    CHECK(setDHWTemperature == 42);
    CHECK_FALSE(setCentralHeatingOn);
    CHECK(boot_count == 8);
    CHECK(preferences.uints["boot_count"] == 8);
}

TEST_CASE("temperature endpoints apply a valid value and persist it under the key setup() reads") {
    Booted b;
    for (const Endpoint& e : temperature_endpoints) {
        CAPTURE(e.path);
        CHECK(server.get(e.path, {{"temperature", "45"}}).code == 200);
        CHECK(*e.setpoint == 45);
        CHECK(preferences.floats.at(e.nvs_key) == 45);
    }
}

TEST_CASE("temperature endpoints reject bad values and leave the setpoint alone") {
    Booted b;
    for (const Endpoint& e : temperature_endpoints) {
        *e.setpoint = 55;
        for (const char* bad : {"0", "100", "-5", "500", "nan", "inf", "abc", ""}) {
            CAPTURE(e.path);
            CAPTURE(bad);
            CHECK(server.get(e.path, {{"temperature", bad}}).code == 400);
            CHECK(*e.setpoint == 55);
        }
        CHECK(server.get(e.path).code == 400);  // missing parameter
        CHECK(preferences.floats.count(e.nvs_key) == 0);
    }
}

TEST_CASE("/set_central_heating") {
    Booted b;
    CHECK(server.get("/set_central_heating", {{"state", "off"}}).code == 200);
    CHECK_FALSE(setCentralHeatingOn);
    CHECK_FALSE(preferences.bools.at("ch_on"));
    CHECK(server.get("/set_central_heating", {{"state", "maybe"}}).code == 400);
    CHECK_FALSE(setCentralHeatingOn);
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

TEST_CASE("/ reports state as JSON") {
    Booted b;
    server.get("/set_boiler_temperature", {{"temperature", "61"}});
    JsonDocument doc = status();
    CHECK(server.sent.type == "application/json");
    CHECK(doc["requested_ch_temp"].as<float>() == 61);
    CHECK(doc["boot_count"].as<int>() == 1);
}
