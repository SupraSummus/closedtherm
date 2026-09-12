// Records routes, serves query args from a map, captures the last response.
#pragma once
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include "Arduino.h"

class WebServer {
public:
    explicit WebServer(int = 80) {}
    void on(const char* path, std::function<void()> handler) { routes[path] = handler; }
    void begin() {}
    // Serves the request a test queued, on the handleClient() call it asked for,
    // so a test can land a /set in the middle of a loop() pass.
    void handleClient() {
        if (pending && --pending_in == 0) {
            auto request = pending;
            pending = nullptr;
            request();
        }
    }

    bool hasArg(const char* name) const { return query.count(name) > 0; }
    String arg(const char* name) const {
        auto it = query.find(name);
        return it == query.end() ? String() : String(it->second.c_str());
    }
    int args() const { return static_cast<int>(query.size()); }
    String argName(int i) const {
        auto it = query.begin();
        std::advance(it, i);
        return String(it->first.c_str());
    }
    void send(int code, const char* type, const String& body) { sent = {code, type, body.c_str()}; }

    // Test side.
    struct Response { int code = 0; std::string type, body; };
    std::map<std::string, std::function<void()>> routes;
    std::map<std::string, std::string> query;
    Response sent;
    std::function<void()> pending;
    int pending_in = 0;  // handleClient() calls until `pending` is served

    Response get(const char* path, std::map<std::string, std::string> args = {}) {
        query = args;
        sent = {};
        routes.at(path)();
        return sent;
    }
};
