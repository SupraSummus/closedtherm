# closedtherm

ESP32 sketch that talks OpenTherm to a boiler and exposes its state over HTTP/JSON.

## Build and flash

```sh
bash install.sh   # esp32 core + libraries, once
bash upload.sh    # compile and flash via /dev/ttyUSB0
bash monitor.sh   # serial monitor
```

`creds.h` (gitignored) must define `ssid` and `password`.

## HTTP API

`GET /` reports everything the sketch knows, as JSON.

`GET /set` changes settings, which are kept in NVS across reboots, and answers with the same JSON.
Each parameter is named after the key it appears under in that JSON.

| parameter | values |
| --- | --- |
| `requested_ch_on` | `on` or `off` |
| `requested_dhw_on` | `on` or `off` |
| `requested_ch_temp` | CH setpoint in degrees C, `0 < t < 100` |
| `requested_dhw_temp` | hot water setpoint in degrees C, `0 < t < 100` |

All are optional, at least one is required, and they can be combined:

```sh
curl 'http://boiler/set?requested_ch_on=on&requested_ch_temp=60&requested_dhw_temp=55'
```

An unknown parameter or a bad value is a `400` that changes nothing, so a request either applies in full or not at all.

## Tests

```sh
bash test.sh      # g++ and make; first run downloads doctest and ArduinoJson (pinned, checksummed)
```

The sketch is tested as is, on the host.
`test/test_sketch.cpp` includes `ardu.ino` and compiles it against `test/fakes/`, which stand in for the Arduino core, WebServer, Preferences, WiFi and OpenTherm; ArduinoJson is the real library.
Tests then drive it the way the ESP32 would: `setup()`, `loop()` with a controlled clock, HTTP handlers through the recorded routes.
They check status codes, NVS keys, reconnect behaviour and the `/` JSON, not the real network stack.

Add tests as `TEST_CASE`s in `test_sketch.cpp`; the Makefile also picks up any new `test/test_*.cpp`.

CI runs the tests and an `esp32:esp32:esp32` compile on pull requests and on pushes to `main`.
