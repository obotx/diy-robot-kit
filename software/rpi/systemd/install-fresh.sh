#!/bin/sh
set -eu
test "$(id -un)" = obotx || { echo "Run as obotx, not root." >&2; exit 1; }
cd "$(dirname "$0")/.."
test "$(pwd -P)" = /home/obotx/ObotX/software/rpi
echo "Keep motor power OFF; connect ARM and BASE logic over USB."
printf 'Type INSTALL to continue: '
read -r answer
test "$answer" = INSTALL || exit 1
sudo apt-get update
sudo apt-get install -y python3-venv python3-pip ffmpeg network-manager dnsmasq-base bluez rfkill unzip
python3 -m venv .venv
sh systemd/install.sh --robot
.venv/bin/python -m app.service_access
sh systemd/configure-camera-network.sh
sudo python3 systemd/prepare-hotspot.py
echo "Installed. Configure cameras and SmartShunt while internet is available."
echo "Then follow docs/hotspot-installation.md. Wi-Fi has NOT been switched."
