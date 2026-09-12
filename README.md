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
Each parameter is named after the key it appears under in that JSON, except the `pi_` ones, which appear under `pi` with that prefix stripped: `pi_kp` sets `pi.kp`.

| parameter | values |
| --- | --- |
| `requested_ch_on` | `on` or `off` |
| `requested_dhw_on` | `on` or `off` |
| `requested_ch_temp` | manual CH setpoint in degrees C, `0 < t < 100` |
| `requested_dhw_temp` | hot water setpoint in degrees C, `0 < t < 100` |
| `ch_temp_source` | which algorithm decides the CH setpoint: `manual` or `pi` |
| `pi_target_temp` | room temperature the PI controller aims for, `5 <= t <= 35` |
| `pi_kp` | proportional gain, `0 <= k <= 100` |
| `pi_ki` | integral gain, `0 <= k <= 100` |
| `pi_integral` | the controller's learned base, `5 <= i <= 80` — settable to seed a tuning run |

All are optional, at least one is required, and they can be combined:

```sh
curl 'http://boiler/set?requested_ch_on=on&requested_ch_temp=60&requested_dhw_temp=55'
```

An unknown parameter or a bad value is a `400` that changes nothing, so a request either applies in full or not at all.

## CH setpoint: manual or PI

`ch_temp_source` names the algorithm the boiler is told. Two so far, and `ardu.ino` says how to add a third:

- `manual` — `requested_ch_temp`, sent as given.
- `pi` — a PI controller that takes the room temperature from the analog sensor and aims it at `pi_target_temp`.

Both run at all times, so `/` shows what the controller would do before it is trusted with the boiler.

The PI algorithm sits in two files, neither of which is the sketch:

- `pi.h` is arithmetic and nothing else — no Arduino, no boiler. It is handed a reading and a clock, and called by whichever of its three states applies.
- `pi_source.h` is that algorithm wired up as a source of CH setpoints: its tunables, what they are called over HTTP, the NVS keys they are saved under, the values they accept, which state the controller belongs in, and what `/` reports. A tunable is one row of `piSettings` and nothing else: that one name is the `/set` parameter, the NVS key and the path under `pi` at once.

`ardu.ino` keeps only what is its own: the sensor, the NVS namespace, the HTTP routes, and the switch that picks between sources.

`pi_ki` is in setpoint degrees per degree of room error per **second**: the controller integrates over the time that actually elapsed, so its tuning does not depend on how long one `loop()` takes.
Output and integral are both clamped to 5–80 °C, and clamping the integral to that same range is what stops it winding up while the output sits at a limit.

Out of charge the controller tracks, held at the value that makes `pi.output` equal the setpoint going out.
Switching to `pi` then changes nothing at that instant, and the controller carries on from the real operating point rather than from wherever an idle integral had drifted.
It cannot track past its own range, so a manual setpoint above 80 °C is matched only as far as 80.
The integral is held again while `requested_ch_on` is `off`, since the boiler cannot answer an error it was never asked to.

```sh
curl 'http://boiler/set?ch_temp_source=pi&pi_target_temp=21&pi_kp=8&pi_ki=0.002'
```

The integral is the slow half of that: it is what the controller learns about the house, and at these gains it takes the better part of a day to climb from its floor to a working flow temperature.
Losing it to a watchdog reset would cost that same day of under-heating, so it goes to NVS while the controller is in charge — at most every ten minutes or so, and only once it has moved by a degree, which in a settled house is almost never.
The wait is jittered by up to two minutes so the writes do not land on a rigid grid.
A reboot picks the integral up where it left off, and `/set` can seed it directly rather than waiting for it to climb.

The gains above are a starting point rather than a tuned result, and so is the sensor's calibration (671 mV at 18 °C, 2 mV per degree).
`/` reports `pi.integral` and `pi.output` for following a tuning run from outside, and `effective_ch_temp` for the number the switch currently selects.
The error is `pi.target_temp` minus `temp_sensor_c`, both of which are there already.
That is not quite the number the boiler holds: setpoints go out every 10 s, or sooner if one moves by 0.5 °C or more.
Without that deadband the PI output, which drifts a little on every pass, would cost an OpenTherm exchange every time.

## Tests

```sh
bash test.sh      # g++ and make; first run downloads doctest and ArduinoJson (pinned, checksummed)
```

The sketch is tested as is, on the host.
`test/test_sketch.cpp` includes `ardu.ino` and compiles it against `test/fakes/`, which stand in for the Arduino core, WebServer, Preferences, WiFi and OpenTherm; ArduinoJson is the real library.
Tests then drive it the way the ESP32 would: `setup()`, `loop()` with a controlled clock, HTTP handlers through the recorded routes.
They check status codes, NVS keys, reconnect behaviour, setpoint refresh timing and the `/` JSON, not the real network stack.

`test/test_pi.cpp` tests `pi.h` on its own instead, which needs none of that: the controller holds no opinion about boilers and is handed its clock, so a test is a few calls and an assertion.
The sketch tests then cover only the wiring — which state the sketch puts the controller in, and that its settings survive `/set`, NVS and `/`, walking `piSettings` rather than naming each one.

Add tests as `TEST_CASE`s in whichever of the two fits; the Makefile picks up any new `test/test_*.cpp`.

CI runs the tests and an `esp32:esp32:esp32` compile on pull requests and on pushes to `main`.
