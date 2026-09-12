set -euo pipefail

arduino-cli --config-file arduino-cli.yaml core update-index
arduino-cli --config-file arduino-cli.yaml core install esp32:esp32

arduino-cli --config-file arduino-cli.yaml lib update-index
# Pinned to the version the host tests build against (test/Makefile).
arduino-cli --config-file arduino-cli.yaml lib install ArduinoJson@7.4.2
#arduino-cli --config-file arduino-cli.yaml lib install OneWire
#arduino-cli --config-file arduino-cli.yaml lib install DallasTemperature
arduino-cli --config-file arduino-cli.yaml lib install "OpenTherm Library"
