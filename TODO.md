# TODO

- Stale OpenTherm data: `/` reports the last values even when the boiler stopped answering; decide what to publish after N failed exchanges, then test it.
- `millis()` wrap-around (49 days) is untested: `unsigned long` is 64-bit on the host, 32-bit on the ESP32. Either build tests with `-m32` or use `uint32_t` for timestamps in the sketch.
- NVS keys (`req_ch_temp`) do not match the API names (`requested_ch_temp`); renaming them needs a migration or every saved setpoint falls back to its default once.
- `StaticJsonDocument` is deprecated in ArduinoJson 7 (which `install.sh` pulls as latest); switch to `JsonDocument` and pin the library version in `install.sh`.
