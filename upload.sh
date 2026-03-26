set -euo pipefail

arduino-cli --config-file arduino-cli.yaml compile -b esp32:esp32:esp32
arduino-cli --config-file arduino-cli.yaml upload -b esp32:esp32:esp32 -p /dev/ttyUSB0
