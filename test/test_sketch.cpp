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
        temp_samples_next = temp_samples_held = 0;
        fake::adc_mv = 671;
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

TEST_CASE("temperature is averaged over the readings taken so far, one per loop") {
    Booted b;
    fake::adc_mv = 701;
    tick(true, 1000);
    JsonDocument doc = status();
    CHECK(doc["temp_sensor_mv"].as<float>() == 701);  // the one reading taken, not it over a full window
    CHECK(doc["temp_sensor_c"].as<float>() == 3);     // degrees follow the average
    fake::adc_mv = 601;
    tick(true, 2000);
    CHECK(status()["temp_sensor_mv"].as<float>() == 651);  // averaged with the first, which still counts
}

TEST_CASE("a reading falls out of the average once the window has moved past it") {
    Booted b;
    fake::adc_mv = 700;
    tick(true, 1000);  // one reading...
    fake::adc_mv = 600;
    for (int i = 1; i < tempSamples; i++) {  // ...then the rest of the window behind it
        tick(true, 1000 + i);
    }
    CHECK(status()["temp_sensor_mv"].as<float>() == 605);  // the 700 still weighs on the average
    tick(true, 2000);
    CHECK(status()["temp_sensor_mv"].as<float>() == 600);  // the newest reading has overwritten it
}

TEST_CASE("/ reports state as JSON") {
    Booted b;
    server.get("/set", {{"requested_ch_temp", "61"}});
    JsonDocument doc = status();
    CHECK(server.sent.type == "application/json");
    CHECK(doc["requested_ch_temp"].as<float>() == 61);
    CHECK(doc["boot_count"].as<int>() == 1);
}
