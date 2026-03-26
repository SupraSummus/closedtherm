set -euo pipefail

arduino-cli --config-file arduino-cli.yaml monitor -p /dev/ttyUSB0 -c baudrate=115200
