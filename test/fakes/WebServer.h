// Records routes, serves query args from a map, captures the last response.
#pragma once
#include <functional>
#include <map>
#include <string>
#include "Arduino.h"

class WebServer {
public:
    explicit WebServer(int = 80) {}
    void on(const char* path, std::function<void()> handler) { routes[path] = handler; }
    void begin() {}
    void handleClient() {}

    bool hasArg(const char* name) const { return args.count(name) > 0; }
    String arg(const char* name) const {
        auto it = args.find(name);
        return it == args.end() ? String() : String(it->second.c_str());
    }
    void send(int code, const char* type, const String& body) { sent = {code, type, body.c_str()}; }

    // Test side.
    struct Response { int code = 0; std::string type, body; };
    std::map<std::string, std::function<void()>> routes;
    std::map<std::string, std::string> args;
    Response sent;

    Response get(const char* path, std::map<std::string, std::string> query = {}) {
        args = query;
        sent = {};
        routes.at(path)();
        return sent;
    }
};
