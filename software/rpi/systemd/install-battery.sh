#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test "$(id -un)" = obotx || { echo "Run as obotx, not root." >&2; exit 1; }
sudo apt-get update
sudo apt-get install -y bluez rfkill
sudo systemctl enable --now bluetooth.service
sudo install -m 644 systemd/obotx-battery-bluetooth.service /etc/systemd/system/obotx-battery-bluetooth.service
sudo systemctl daemon-reload
sudo systemctl enable --now obotx-battery-bluetooth.service
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m app.configure_battery
sudo systemctl restart obotx-rpi-control
echo "Check http://rpi-control.local:8000/api/battery for fresh SmartShunt data."
