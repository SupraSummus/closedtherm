#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <OpenTherm.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "pi_source.h"
#include "thermometer.h"

#define RW_MODE false
#define RO_MODE true

const int inPin = 14;  // for Arduino, 4 for ESP8266 (D2), 21 for ESP32
const int outPin = 27; // for Arduino, 5 for ESP8266 (D1), 22 for ESP32
OpenTherm ot(inPin, outPin);

const int tempSensorPin = 35; // analog pin for temperature sensor
Thermometer thermometer(tempSensorPin);

// The sensor's one tunable, in seconds; what the number means and what it
// accepts are thermometer.h's. The one string is the /set parameter, the NVS key
// and the field / reports at once, and it sits exactly on the cap NVS puts on a
// key, so the check keeps it there: one character more and Preferences would
// write nothing and the setting would revert at the next reboot.
const char tempTauName[] = "temp_sensor_tau";
static_assert(sizeof(tempTauName) - 1 <= 15, "NVS caps a key at 15 characters");

extern const char* ssid;
extern const char* password;
#include "creds.h"

WebServer server(80);

bool readCentralHeatingOn = false;
bool readHotWaterOn = false;
bool readFlameOn = false;
OpenThermResponseStatus responseStatus = OpenThermResponseStatus::NONE;
uint32_t response_ts = 0;
float readBoilerTemperature = 0.0;
float readPressure = 0.0;
float readReturnTemperature = 0.0;
float readModulation = 0.0;
float readDHWTemperature = 0.0;
unsigned char readFault = 0;

bool setCentralHeatingOn = true;
bool setHotWaterOn = true;
float setBoilerTemperature = 60.0;
float setDHWTemperature = 55.0;

// Which algorithm decides the CH setpoint. To add one: append an enumerator,
// append its API name below, and answer the new case in boilerTemperatureTarget()
// and centralHeatingDemand(), which are the two questions a source answers.
// The number is what goes to NVS, so append rather than renumber, or a saved
// setting comes back as a different algorithm.
enum ChTempSource {
    CH_TEMP_MANUAL = 0,
    CH_TEMP_PI = 1,
};

// The name each source goes by in the HTTP API, indexed by the enum above — an
// enumerator with no name here reads past the end, and nothing catches that but
// this comment.
const char* const chTempSourceNames[] = {"manual", "pi"};
const int chTempSourceCount = sizeof(chTempSourceNames) / sizeof(chTempSourceNames[0]);

ChTempSource setChTempSource = CH_TEMP_MANUAL;

// Room temperature in, CH setpoint out. What it is and how it is wired to the
// boiler are both in pi_source.h; here it is one of the algorithms the switch
// above can pick, and its output is a flow temperature in degrees C.
PISource pi;

// state of pushSetpoints(); NAN means nothing was sent yet, so the first pass pushes
const uint32_t setpointInterval = 10000; // 10 s between refreshes
const float setpointEpsilon = 0.5;  // smallest change worth an early write, so the
                                    // PI output does not write on every pass
uint32_t lastSetpointSent = 0;
float sentBoilerTemperature = NAN;
float sentDHWTemperature = NAN;

Preferences preferences;

uint32_t lastWifiConnected = 0;
int wifi_reconnects = 0;

uint32_t boot_count = 0;

// Looks a source up by the name /set was given, so an unknown name is a 400
// rather than a silent fall back to manual.
bool parseChTempSource(const String& value, ChTempSource* source) {
    for (int i = 0; i < chTempSourceCount; i++) {
        if (value == chTempSourceNames[i]) {
            *source = static_cast<ChTempSource>(i);
            return true;
        }
    }
    return false;
}

// Every name, for the error a rejected one answers with.
String chTempSourceNameList() {
    String list = chTempSourceNames[0];
    for (int i = 1; i < chTempSourceCount; i++) {
        list = list + ", " + chTempSourceNames[i];
    }
    return list;
}

// The CH setpoint the boiler is actually asked for. Answer every source here:
// leaving one out is a -Wswitch warning, not a setpoint that quietly reads manual.
float boilerTemperatureTarget() {
    switch (setChTempSource) {
        case CH_TEMP_PI:
            return pi.setpoint();
        case CH_TEMP_MANUAL:
            break;
    }
    return setBoilerTemperature;
}

// Whether the boiler is told to heat at all, which is the setpoint's other half:
// requested_ch_on has to allow it, and the source in charge has to want it.
// pi_source.h says why a source ever asks to be off rather than for less.
//
// Answer every source here, as boilerTemperatureTarget() does. Manual has no
// opinion to answer with: the number is the operator's, and so is the switch.
bool centralHeatingDemand() {
    if (!setCentralHeatingOn) {
        return false;
    }
    switch (setChTempSource) {
        case CH_TEMP_PI:
            return pi.heatDemand;
        case CH_TEMP_MANUAL:
            break;
    }
    return true;
}

void IRAM_ATTR handleInterrupt()
{
    ot.handleInterrupt();
}

void handleRoot() {
    Serial.println("Handling HTTP request");

    JsonDocument doc;
    doc["ch_on"] = readCentralHeatingOn;
    doc["dhw_on"] = readHotWaterOn;
    doc["flame_on"] = readFlameOn;
    doc["response_status"] = OpenTherm::statusToString(responseStatus);
    doc["response_ts"] = response_ts;

    doc["ch_temp"] = readBoilerTemperature;
    doc["requested_ch_on"] = setCentralHeatingOn;
    doc["requested_dhw_on"] = setHotWaterOn;
    doc["requested_ch_temp"] = setBoilerTemperature;
    doc["requested_dhw_temp"] = setDHWTemperature;

    doc["ch_temp_source"] = chTempSourceNames[setChTempSource];
    doc["effective_ch_temp"] = boilerTemperatureTarget();
    doc["ch_demand"] = centralHeatingDemand();
    pi.report(doc);

    doc["pressure"] = readPressure;
    doc["return_temp"] = readReturnTemperature;
    doc["modulation"] = readModulation;
    doc["dhw_temp"] = readDHWTemperature;
    doc["fault"] = readFault;
    doc["temp_sensor_mv"] = thermometer.millivolts;
    doc["temp_sensor_c"] = thermometer.celsius;
    doc[tempTauName] = thermometer.timeConstant;

    doc["wifi_reconnects"] = wifi_reconnects;
    doc["boot_count"] = boot_count;

    String output;
    serializeJsonPretty(doc, output);

    server.send(200, "application/json", output);
}

// Saves one setting, opening and closing NVS around the write.
void savePreference(const char* key, bool value) {
    preferences.begin("opentherm", RW_MODE);
    preferences.putBool(key, value);
    preferences.end();
}

void savePreference(const char* key, float value) {
    preferences.begin("opentherm", RW_MODE);
    preferences.putFloat(key, value);
    preferences.end();
}

void savePreference(const char* key, uint32_t value) {
    preferences.begin("opentherm", RW_MODE);
    preferences.putUInt(key, value);
    preferences.end();
}

// String::toFloat() reads "abc" as 0 and stops at the first junk character, which
// would let a typo through wherever 0 is inside the allowed range — as it is for
// the PI gains. Parse the whole string or reject it.
bool parseFloat(const String& value, float* parsed) {
    const char* start = value.c_str();
    char* end = NULL;
    float result = strtof(start, &end);
    if (end == start || *end != '\0' || isnan(result) || isinf(result)) {
        return false;
    }
    *parsed = result;
    return true;
}

// /set?requested_ch_on=on&requested_ch_temp=60 — parameters are optional, but
// at least one is required, and they are named after the keys / reports them
// under. The whole request is checked before anything is applied, so it either
// takes effect in full or not at all. Answers like /.
void handleSet() {
    if (server.args() == 0) {
        server.send(400, "text/plain", "No parameters given");
        return;
    }
    for (int i = 0; i < server.args(); i++) {
        String name = server.argName(i);
        String value = server.arg(name.c_str());
        String error;
        float number = 0.0;
        if (name == "requested_ch_on" || name == "requested_dhw_on") {
            if (!(value == "on" || value == "off")) {
                error = "Invalid " + name + ", expected on or off";
            }
        } else if (name == "requested_ch_temp" || name == "requested_dhw_temp") {
            if (!(parseFloat(value, &number) && number > 0.0 && number < 100.0)) {
                error = "Invalid " + name + ", expected 0 < t < 100";
            }
        } else if (name == tempTauName) {
            if (!(parseFloat(value, &number) && number >= Thermometer::minTimeConstant &&
                  number <= Thermometer::maxTimeConstant)) {
                error = "Invalid " + name + ", expected " +
                        String(Thermometer::minTimeConstant) + " to " +
                        String(Thermometer::maxTimeConstant);
            }
        } else if (name == "ch_temp_source") {
            ChTempSource source;
            if (!parseChTempSource(value, &source)) {
                error = "Invalid " + name + ", expected one of " + chTempSourceNameList();
            }
        } else if (const PISetting* setting = pi.setting(name)) {
            if (!(parseFloat(value, &number) && number >= setting->min && number <= setting->max)) {
                error = "Invalid " + name + ", expected " + String(setting->min) +
                        " to " + String(setting->max);
            }
        } else {
            error = "Unknown parameter " + name;
        }
        if (error.length() > 0) {
            server.send(400, "text/plain", error);
            return;
        }
    }

    if (server.hasArg("requested_ch_on")) {
        setCentralHeatingOn = server.arg("requested_ch_on") == "on";
        savePreference("ch_on", setCentralHeatingOn);
    }
    if (server.hasArg("requested_dhw_on")) {
        setHotWaterOn = server.arg("requested_dhw_on") == "on";
        savePreference("dhw_on", setHotWaterOn);
    }
    if (server.hasArg("requested_ch_temp")) {
        setBoilerTemperature = server.arg("requested_ch_temp").toFloat();
        savePreference("req_ch_temp", setBoilerTemperature);
    }
    if (server.hasArg("requested_dhw_temp")) {
        setDHWTemperature = server.arg("requested_dhw_temp").toFloat();
        savePreference("req_dhw_temp", setDHWTemperature);
    }
    if (server.hasArg(tempTauName)) {
        thermometer.setTimeConstant(server.arg(tempTauName).toFloat());  // checked above
        savePreference(tempTauName, thermometer.timeConstant);
    }
    if (server.hasArg("ch_temp_source")) {
        parseChTempSource(server.arg("ch_temp_source"), &setChTempSource);  // checked above
        savePreference("ch_temp_src", static_cast<uint32_t>(setChTempSource));
    }
    for (int i = 0; i < piSettingCount; i++) {
        if (server.hasArg(piSettings[i].name)) {
            pi.set(piSettings[i], server.arg(piSettings[i].name).toFloat());
            savePreference(piSettings[i].name, pi.value(piSettings[i]));
        }
    }
    handleRoot();
}

void configure_wifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    WiFi.setAutoReconnect(true);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Start");

    ot.begin(handleInterrupt); // for ESP ot.begin(); without interrupt handler can be used

    configure_wifi();

    // configure server
    server.on("/", handleRoot);
    server.on("/set", handleSet);
    server.begin();

    thermometer.begin();

    // read saved CH temperature setpoint
    bool ok = preferences.begin("opentherm", RO_MODE);
    Serial.println("Preferences opened: " + String(ok ? "OK" : "Failed"));
    setCentralHeatingOn = preferences.getBool("ch_on", setCentralHeatingOn);
    setHotWaterOn = preferences.getBool("dhw_on", setHotWaterOn);
    setBoilerTemperature = preferences.getFloat("req_ch_temp", setBoilerTemperature);
    setDHWTemperature = preferences.getFloat("req_dhw_temp", setDHWTemperature);
    thermometer.setTimeConstant(preferences.getFloat(tempTauName, thermometer.timeConstant));
    // A number no longer on the list means NVS holds a source this build does not
    // have, after a downgrade or a renumbering; fall back instead of indexing past
    // the names.
    uint32_t storedSource = preferences.getUInt("ch_temp_src", static_cast<uint32_t>(setChTempSource));
    setChTempSource = storedSource < static_cast<uint32_t>(chTempSourceCount)
                          ? static_cast<ChTempSource>(storedSource) : CH_TEMP_MANUAL;
    pi.load(preferences);
    boot_count = preferences.getUInt("boot_count", boot_count);
    boot_count++;
    preferences.end();

    // save updated boot count
    preferences.begin("opentherm", RW_MODE);
    preferences.putUInt("boot_count", boot_count);
    preferences.end();

    pi.start(millis());
}

// Sends both setpoints to the boiler every setpointInterval, or right away when
// one of them moves by setpointEpsilon or more. Called on every loop() pass.
// The PI output drifts by a fraction of a degree between passes, so without that
// deadband it would write on every pass; a smaller change still goes out with
// the next refresh.
void pushSetpoints() {
    float boilerTemperature = boilerTemperatureTarget();
    // NAN (nothing sent yet) makes both comparisons false, so the first pass counts as changed.
    bool changed = !(fabs(boilerTemperature - sentBoilerTemperature) < setpointEpsilon) ||
                   !(fabs(setDHWTemperature - sentDHWTemperature) < setpointEpsilon);
    if (!changed && millis() - lastSetpointSent < setpointInterval) {
        return;
    }
    sentBoilerTemperature = boilerTemperature;
    sentDHWTemperature = setDHWTemperature;
    lastSetpointSent = millis();

    bool ok = ot.setBoilerTemperature(boilerTemperature);
    Serial.println("Set Boiler Temperature: " + String(ok ? "OK" : "Failed"));
    server.handleClient();

    ok = ot.setDHWSetpoint(setDHWTemperature);
    Serial.println("Set DHW Temperature: " + String(ok ? "OK" : "Failed"));
    server.handleClient();
}

void loop()
{
    // check wifi connection and reconnect if needed
    if (WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        if (now - lastWifiConnected > 30000) { // try to reconnect every 30 seconds
            Serial.println("WiFi not connected, trying to reconnect...");
            WiFi.disconnect(true);
            configure_wifi();
            lastWifiConnected = now;
            wifi_reconnects++;
        }
    } else {
        lastWifiConnected = millis();
    }

    thermometer.sample();
    Serial.println("Temperature sensor value: " + String(thermometer.millivolts) + " mV, " +
                   String(thermometer.celsius) + " C");
    server.handleClient();

    // Run the controller whether or not it is the one driving, so / reports what
    // it would do before anyone trusts it with the boiler. boilerTemperatureTarget()
    // answers its own output only when the switch is on it, which is the one case
    // that does not read the argument, so there is no circularity here.
    //
    // What goes in is the operator's switch, not centralHeatingDemand(): the
    // controller's own half of that demand is what this call works out.
    pi.update(setChTempSource == CH_TEMP_PI, setCentralHeatingOn, thermometer.celsius,
              boilerTemperatureTarget(), millis());
    if (pi.integralDueToSave(millis())) {
        savePreference(piIntegralName, pi.control.integral);
    }
    Serial.println("PI error " + String(pi.control.error) + " C, output " +
                   String(pi.control.output) + " C, CH setpoint from " +
                   chTempSourceNames[setChTempSource] + ", CH demand " +
                   String(centralHeatingDemand() ? "on" : "off"));
    server.handleClient();

    // Set/Get Boiler Status
    unsigned long response = ot.setBoilerStatus(centralHeatingDemand(), setHotWaterOn, false, false, false);
    responseStatus = ot.getLastResponseStatus();
    readCentralHeatingOn = ot.isCentralHeatingActive(response);
    readHotWaterOn = ot.isHotWaterActive(response);
    readFlameOn = ot.isFlameOn(response);
    response_ts = millis();
    server.handleClient();

    pushSetpoints();

    // Get Boiler Temperature
    readBoilerTemperature = ot.getBoilerTemperature();
    Serial.println("CH temperature is " + String(readBoilerTemperature) + " degrees C");
    server.handleClient();

    // ch pressure
    readPressure = ot.getPressure();
    Serial.println("CH pressure is " + String(readPressure) + " bar");
    server.handleClient();

    // return temperature
    readReturnTemperature = ot.getReturnTemperature();
    Serial.println("Return temperature is " + String(readReturnTemperature) + " degrees C");
    server.handleClient();

    // modulation
    readModulation = ot.getModulation();
    Serial.println("Modulation is " + String(readModulation) + " %");
    server.handleClient();

    // dhw temperature
    readDHWTemperature = ot.getDHWTemperature();
    Serial.println("DHW temperature is " + String(readDHWTemperature) + " degrees C");
    server.handleClient();

    // fault code
    readFault = ot.getFault();
    Serial.println("Fault code is " + String(readFault));
    server.handleClient();

}
