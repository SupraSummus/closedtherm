#include <Arduino.h>
#include <OpenTherm.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define RW_MODE false
#define RO_MODE true

const int inPin = 14;  // for Arduino, 4 for ESP8266 (D2), 21 for ESP32
const int outPin = 27; // for Arduino, 5 for ESP8266 (D1), 22 for ESP32
OpenTherm ot(inPin, outPin);

const int tempSensorPin = 35; // analog pin for temperature sensor

extern const char* ssid;
extern const char* password;
#include "creds.h"

WebServer server(80);

bool readCentralHeatingOn = false;
bool readHotWaterOn = false;
bool readFlameOn = false;
OpenThermResponseStatus responseStatus = OpenThermResponseStatus::NONE;
unsigned long response_ts = 0;
float readBoilerTemperature = 0.0;
float readPressure = 0.0;
float readReturnTemperature = 0.0;
float readModulation = 0.0;
float readDHWTemperature = 0.0;
unsigned char readFault = 0;
float temp_mv = 0.0;
float temp_c = 0.0;

// The sensor is noisy, so loop() feeds one reading per iteration into this ring
// buffer and reports the average of it — a window of the last tempSamples
// iterations, each as long as the OpenTherm exchanges below take.
const int tempSamples = 20;
float temp_samples[tempSamples];
int temp_samples_next = 0;  // slot the next reading goes into
int temp_samples_held = 0;  // slots filled so far, until the buffer wraps

bool setCentralHeatingOn = true;
bool setHotWaterOn = true;
float setBoilerTemperature = 60.0;
float setDHWTemperature = 55.0;

Preferences preferences;

unsigned long lastWifiConnected = 0;
int wifi_reconnects = 0;

uint32_t boot_count = 0;

// Samples the sensor into the buffer and returns the average of the window.
// Until the buffer fills it averages just the readings taken so far, so the
// first values after a boot are not dragged towards zero by the empty slots.
float sampleTempMillivolts() {
    temp_samples[temp_samples_next] = analogReadMilliVolts(tempSensorPin);
    temp_samples_next = (temp_samples_next + 1) % tempSamples;
    if (temp_samples_held < tempSamples) {
        temp_samples_held++;
    }
    float sum = 0.0;
    for (int i = 0; i < temp_samples_held; i++) {
        sum += temp_samples[i];
    }
    return sum / temp_samples_held;
}

void IRAM_ATTR handleInterrupt()
{
    ot.handleInterrupt();
}

void handleRoot() {
    Serial.println("Handling HTTP request");

    StaticJsonDocument<1024> doc;
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
    doc["pressure"] = readPressure;
    doc["return_temp"] = readReturnTemperature;
    doc["modulation"] = readModulation;
    doc["dhw_temp"] = readDHWTemperature;
    doc["fault"] = readFault;
    doc["temp_sensor_mv"] = temp_mv;
    doc["temp_sensor_c"] = temp_c;

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
        if (name == "requested_ch_on" || name == "requested_dhw_on") {
            if (!(value == "on" || value == "off")) {
                error = "Invalid " + name + ", expected on or off";
            }
        } else if (name == "requested_ch_temp" || name == "requested_dhw_temp") {
            float temp = value.toFloat();
            if (!(temp > 0.0 && temp < 100.0)) {  // both comparisons also reject nan and inf
                error = "Invalid " + name + ", expected 0 < t < 100";
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

    // temperature sensor
    pinMode(tempSensorPin, INPUT);
    analogSetPinAttenuation(tempSensorPin, ADC_0db); // 0-1V range

    // read saved CH temperature setpoint
    bool ok = preferences.begin("opentherm", RO_MODE);
    Serial.println("Preferences opened: " + String(ok ? "OK" : "Failed"));
    setCentralHeatingOn = preferences.getBool("ch_on", setCentralHeatingOn);
    setHotWaterOn = preferences.getBool("dhw_on", setHotWaterOn);
    setBoilerTemperature = preferences.getFloat("req_ch_temp", setBoilerTemperature);
    setDHWTemperature = preferences.getFloat("req_dhw_temp", setDHWTemperature);
    boot_count = preferences.getUInt("boot_count", boot_count);
    boot_count++;
    preferences.end();

    // save updated boot count
    preferences.begin("opentherm", RW_MODE);
    preferences.putUInt("boot_count", boot_count);
    preferences.end();

}

void loop()
{
    // check wifi connection and reconnect if needed
    if (WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
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

    // read temperature sensor
    temp_mv = sampleTempMillivolts();
    temp_c = 18.0 - (temp_mv - 671.0) / 2.0;
    Serial.println("Temperature sensor value: " + String(temp_mv) + " mV, " + String(temp_c) + " C");
    server.handleClient();

    // Set/Get Boiler Status
    unsigned long response = ot.setBoilerStatus(setCentralHeatingOn, setHotWaterOn, false, false, false);
    responseStatus = ot.getLastResponseStatus();
    readCentralHeatingOn = ot.isCentralHeatingActive(response);
    readHotWaterOn = ot.isHotWaterActive(response);
    readFlameOn = ot.isFlameOn(response);
    response_ts = millis();
    server.handleClient();

    // Set Boiler Temperature
    bool ok = ot.setBoilerTemperature(setBoilerTemperature);
    Serial.println("Set Boiler Temperature: " + String(ok ? "OK" : "Failed"));
    server.handleClient();

    ok = ot.setDHWSetpoint(setDHWTemperature);
    Serial.println("Set DHW Temperature: " + String(ok ? "OK" : "Failed"));
    server.handleClient();

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
