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

bool setCentralHeatingOn = true;
float setBoilerTemperature = 60.0;
float setDHWTemperature = 55.0;

Preferences preferences;

unsigned long lastWifiConnected = 0;
int wifi_reconnects = 0;

uint32_t boot_count = 0;

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

void handleSetCentralHeating() {
    if (server.hasArg("state")) {
        String state = server.arg("state");
        if (state == "on") {
            setCentralHeatingOn = true;
        } else if (state == "off") {
            setCentralHeatingOn = false;
        } else {
            server.send(400, "text/plain", "Invalid state value");
            return;
        }
        preferences.begin("opentherm", RW_MODE);
        preferences.putBool("ch_on", setCentralHeatingOn);
        preferences.end();
        server.send(200, "text/plain", "Central heating turned " + state);
    } else {
        server.send(400, "text/plain", "Missing 'state' parameter");
    }
}

void handleSetBoilerTemperature() {
    if (server.hasArg("temperature")) {
        float temp = server.arg("temperature").toFloat();
        if (setBoilerTemperature > 0.0 && setBoilerTemperature < 100.0) {
            setBoilerTemperature = temp;
            bool ok = preferences.begin("opentherm", RW_MODE);
            Serial.println("Preferences opened for writing: " + String(ok ? "OK" : "Failed"));
            int result = preferences.putFloat("req_ch_temp", setBoilerTemperature);
            Serial.println("Preferences write result: " + String(result));
            preferences.end();
            server.send(200, "text/plain", "Boiler temperature set to " + String(setBoilerTemperature));
        } else {
            server.send(400, "text/plain", "Invalid temperature value");
        }
    } else {
        server.send(400, "text/plain", "Missing 'temperature' parameter");
    }
}

void handleSetDHWTemperature() {
    if (server.hasArg("temperature")) {
        float temp = server.arg("temperature").toFloat();
        if (setDHWTemperature > 0.0 && setDHWTemperature < 100.0) {
            setDHWTemperature = temp;
            bool ok = preferences.begin("opentherm", RW_MODE);
            Serial.println("Preferences opened for writing: " + String(ok ? "OK" : "Failed"));
            int result = preferences.putFloat("req_dhw_temp", setDHWTemperature);
            Serial.println("Preferences write result: " + String(result));
            preferences.end();
            server.send(200, "text/plain", "DHW temperature set to " + String(setDHWTemperature));
        } else {
            server.send(400, "text/plain", "Invalid temperature value");
        }
    } else {
        server.send(400, "text/plain", "Missing 'temperature' parameter");
    }
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
    server.on("/set_central_heating", handleSetCentralHeating);
    server.on("/set_boiler_temperature", handleSetBoilerTemperature);
    server.on("/set_dhw_temperature", handleSetDHWTemperature);
    server.begin();

    // temperature sensor
    pinMode(tempSensorPin, INPUT);
    analogSetPinAttenuation(tempSensorPin, ADC_0db); // 0-1V range

    // read saved CH temperature setpoint
    bool ok = preferences.begin("opentherm", RO_MODE);
    Serial.println("Preferences opened: " + String(ok ? "OK" : "Failed"));
    setCentralHeatingOn = preferences.getBool("ch_on", setCentralHeatingOn);
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
    int sensorValue = 0;
    int n_samples = 20;
    for (int i = 0; i < n_samples; i++) {
        sensorValue += analogReadMilliVolts(tempSensorPin);
        delay(1);
    }
    temp_mv = 1.0 * sensorValue / n_samples;
    temp_c = 18.0 - (temp_mv - 671.0) / 2.0;
    Serial.println("Temperature sensor value: " + String(temp_mv) + " mV, " + String(temp_c) + " C");
    server.handleClient();

    // Set/Get Boiler Status
    bool enableCentralHeating = setCentralHeatingOn;
    bool enableHotWater = true;
    unsigned long response = ot.setBoilerStatus(enableCentralHeating, enableHotWater, false, false, false);
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
