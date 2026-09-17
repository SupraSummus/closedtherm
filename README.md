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
| `temp_sensor_tau` | time constant of the room sensor's low-pass, in seconds, `0 <= t <= 3600` — `0` switches it off |
| `ch_temp_source` | which algorithm decides the CH setpoint: `manual` or `pi` |
| `pi_target_temp` | room temperature the PI controller aims for, `5 <= t <= 35` |
| `pi_kp` | proportional gain, `0.1 <= k <= 100` — strictly positive, see below |
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

`ardu.ino` keeps only what is its own: the sensor pin, the NVS namespace, the HTTP routes, and the switch that picks between sources.

### Asking for no heat

Water arriving colder than the room it is sent to takes heat out of the house rather than putting it in, so a setpoint under the room temperature is not a small demand but the wrong sign, and a boiler told to hold one runs its own minimum and cycles against it instead.
So a source that would ask for less than the room asks to be switched off instead, and `ardu.ino` clears the CH enable bit: `ch_demand` on `/` is what the boiler is told, and `requested_ch_on` is still the switch that overrides it.
`manual` is left alone, since the number there is the operator's and so is the switch, and hot water is untouched either way.

The criterion is the room temperature itself rather than a configured floor, because it is the one point in the range where the sign of the heat flow changes, and everything above it — the boiler's own minimum setpoint, its minimum modulation, how it cycles — is the boiler's business and not a number this sketch can know.

Off is `hold()`, the same state `requested_ch_on=off` puts the controller in, so driving stops near where the output meets the reading and the integral parks a little above the room.
Which is only safe because of the floor under the integral, below — without it, off would be a state the controller could not leave.
The demand may change state at most once every ten minutes, which bounds the boiler to one cycle per twenty however the crossing is being wandered over — sensor noise, a `pi_kp` that answers the room too hard, or the loop closing faster than the house can answer.
That is a time rather than a band of degrees on purpose: a band would bias where the room settles, while a dwell only limits how often the answer may change, not what it is.
It is not a measured need — the median and the ten-minute low-pass leave the reading smooth enough that the crossing should be slow on its own — so treat it as the bound on a case nobody has seen rather than a cure for one somebody has.
It costs the integral a little overshoot past the crossing, since driving carries on until the change is allowed to land: `pi_ki` times the error times the dwell, which at these gains is about a degree.

`pi.heat_demand` on `/` is the controller's own half of that answer, and it is reported whether or not the controller is in charge.
Out of charge it goes with the tracked output, so it says whether whatever is driving is above the room rather than what the controller would do instead — there is no answering that second question while the integral is busy tracking.

`pi_ki` is in setpoint degrees per degree of room error per **second**: the controller integrates over the time that actually elapsed, so its tuning does not depend on how long one `loop()` takes.
The output is clamped to 5–80 °C, and the integral to the same range with `temp_sensor_c` as its floor — clamping it rather than letting it run is what stops it winding up while the output sits at a limit.
The room is the floor because the integral is the flow temperature the house needs before the error moves it, and one below the room is not a small base but a base that cannot heat.
It is also what keeps the switch-off above from closing on itself: off is `hold()`, which does not integrate, so an integral under the room would be one the controller could never raise.
With the floor, a room at or below `pi_target_temp` always comes out asking for heat, whatever the integral was — which is the property to check if this ever looks wrong.

That property is why `pi_kp` has to be strictly positive, and why 0 is a `400` where `pi_ki` takes it happily.
The demand reads the sign of the error out of the output, and at a gain of zero the output is the integral alone, which the floor pins to the room exactly — never above it.
The demand would then never come out on, holding would keep the integral where it was, and the heating would be off for good with `/` reporting a setpoint equal to the room.
A sweep of 12192 states (targets 5–35, rooms −5 to 40, integrals across the range) finds no such state at any positive gain, and every state below target stuck at a gain of zero.

Out of charge the controller tracks, held at the value that makes `pi.output` equal the setpoint going out.
Switching to `pi` then changes nothing at that instant, and the controller carries on from the real operating point rather than from wherever an idle integral had drifted.
It cannot track past its own range, so a manual setpoint above 80 °C is matched only as far as 80, and one below the room only as far as the room puts the integral — which is a setpoint that meant "off" anyway, and still does once the controller takes over.
The integral is held again while the boiler is not heating, whether because `requested_ch_on` is `off` or because the controller itself asked for it — either way the boiler cannot answer an error it was never asked to.

```sh
curl 'http://boiler/set?ch_temp_source=pi&pi_target_temp=21&pi_kp=8&pi_ki=0.002'
```

The integral is the slow half of that: it is what the controller learns about the house, and at these gains the last few degrees of the climb take hours, since the climb slows as the error it feeds on shrinks.
The floor gets it started — a controller booting with an empty NVS lands on the room rather than on 5 — but not to the right number.
Losing it to a watchdog reset would cost those hours in under-heating, so it goes to NVS while the controller is in charge — at most every ten minutes or so, and only once it has moved by a degree, which through the heating season is almost never.
The wait is jittered by up to two minutes so the writes do not land on a rigid grid.
A reboot picks the integral up where it left off, and `/set` can seed it directly rather than waiting for it to climb.

The gains above are a starting point rather than a tuned result, and so is the sensor's calibration in `thermometer.h` (671 mV at 18 °C, 2 mV per degree).
`/` reports `pi.integral` and `pi.output` for following a tuning run from outside, and `effective_ch_temp` for the number the switch currently selects.
The error is `pi.target_temp` minus `temp_sensor_c`, both of which are there already.
That is not quite the number the boiler holds: setpoints go out every 10 s, or sooner if one moves by 0.5 °C or more.
Without that deadband the PI output, which drifts a little on every pass, would cost an OpenTherm exchange every time.

## Room sensor

The room temperature is a transistor junction on an analog pin.
`thermometer.h` owns it end to end, from setting the pin up through reading it to degrees; `ardu.ino` owns the one instance and says which pin.

Two independent filters sit between the ADC and `temp_sensor_mv` / `temp_sensor_c`, each against a different kind of disturbance.

The median is against errors in the reading itself: the raw value is noisy in a way that looks like short one-sided dips, most likely the supply rail and the ADC reference sagging under the radio's current bursts.
Each `loop()` pass takes one reading and the median of the last 16 goes on: a dip lands on one pass or none, and the median drops it however long it lasted, where an average would follow it.

The low-pass is against the temperature itself moving briefly: a draught, a door, someone standing next to the sensor.
Those are real readings the median has no reason to drop, and the controller should not chase them.
It is first order, integrated over the `millis()` that actually elapsed, so like `pi_ki` it does not depend on how long one `loop()` takes.
Until sampling has been going as long as the time constant, the time it has been going is the constant instead, which makes the filter the plain mean of every median so far.
The first reading after a boot has been seen a dozen degrees off, and this way it counts for one pass rather than seeding the whole constant.

How briefly the room has to move to be worth ignoring is a property of the room, so the constant is a setting rather than a constant: `temp_sensor_tau`, in seconds, ten minutes by default.
Since the warm-up above is how long sampling has been going and not the constant itself, a new setting is in force from the next pass, whether it is longer or shorter than the old one.
`0` switches the low-pass off, which leaves `temp_sensor_mv` the plain median: one way to see what the low-pass is smoothing away.
A value outside the band coming back from NVS is clamped into it, since a negative constant would leave the filter weight negative or unbounded rather than merely wrong.

## Tests

```sh
bash test.sh      # g++ and make; first run downloads doctest and ArduinoJson (pinned, checksummed)
```

The sketch is tested as is, on the host.
`test/test_sketch.cpp` includes `ardu.ino` and compiles it against `test/fakes/`, which stand in for the Arduino core, WebServer, Preferences, WiFi and OpenTherm; ArduinoJson is the real library.
Tests then drive it the way the ESP32 would: `setup()`, `loop()` with a controlled clock, HTTP handlers through the recorded routes.
Timestamps are `uint32_t` on the host as on the board, so `millis()` wraps in the tests where it wraps after 49 days on the ESP32.
They check status codes, NVS keys, reconnect behaviour, setpoint refresh timing and the `/` JSON, not the real network stack.

`test/test_pi.cpp` tests `pi.h` on its own instead, which needs none of that: the controller holds no opinion about boilers and is handed its clock, so a test is a few calls and an assertion.
`test/test_thermometer.cpp` tests `thermometer.h` on its own against the fake ADC, which a test can hand a sequence of readings.
The sketch tests then cover only the wiring — which state the sketch puts the controller in, that its settings survive `/set`, NVS and `/`, walking `piSettings` rather than naming each one, and that a `loop()` pass reads the sensor and `/` reports it.

Add tests as `TEST_CASE`s in whichever of the two fits; the Makefile picks up any new `test/test_*.cpp`.

CI runs the tests and an `esp32:esp32:esp32` compile on pull requests and on pushes to `main`.
