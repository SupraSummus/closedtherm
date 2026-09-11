#pragma once

enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum { WIFI_STA = 1 };

struct FakeWiFi {
    void mode(int) {}
    void begin(const char*, const char*) { begins++; }
    void setAutoReconnect(bool) {}
    void disconnect(bool = false) { disconnects++; }
    int status() { return status_; }

    int status_ = WL_DISCONNECTED;
    int begins = 0;
    int disconnects = 0;
};
inline FakeWiFi WiFi;
