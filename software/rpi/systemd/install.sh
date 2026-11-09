#!/bin/sh
set -eu
case "${1:-}" in
  ""|--auto|--zl-test|--service-test) ;;
  --arm-test) test "$#" -eq 2 || { echo "Usage: sh systemd/install.sh --arm-test MAX_Z_DIFFERENCE_MM" >&2; exit 1; } ;;
  --robot) test "$#" -le 2 || { echo "Usage: sh systemd/install.sh --robot [MAX_Z_DIFFERENCE_MM]" >&2; exit 1; } ;;
  *) echo "Usage: sh systemd/install.sh [--auto|--zl-test|--service-test|--arm-test MAX_Z_DIFFERENCE_MM]" >&2; exit 1 ;;
esac
cd "$(dirname "$0")/.."
test "$(pwd -P)" = /home/obotx/ObotX/software/rpi || {
  echo "Install this release in /home/obotx/ObotX/software/rpi first." >&2
  exit 1
}
test -x .venv/bin/python
if ! command -v ffmpeg >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y ffmpeg
fi
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m pip check
if [ "${1:-}" = --zl-test ]; then
  sudo systemctl stop obotx-rpi-control
  .venv/bin/python -m app.configure_zl --confirmed-isolated-zl --output /home/obotx/ObotX/config/stm32-zl-test.json
  if [ -f /etc/obotx-rpi-control-mode.env ]; then
    sudo cp /etc/obotx-rpi-control-mode.env "/etc/obotx-rpi-control-mode.env.bak-$(date +%Y%m%d-%H%M%S)"
  fi
  sudo install -m 644 systemd/zl-test.env /etc/obotx-rpi-control-mode.env
fi
if [ "${1:-}" = --arm-test ]; then
  sudo systemctl stop obotx-rpi-control
  .venv/bin/python -m app.configure_arm --max-z-difference-mm "$2" --output /home/obotx/ObotX/config/stm32-arm-test.json
  if [ -f /etc/obotx-rpi-control-mode.env ]; then
    sudo cp /etc/obotx-rpi-control-mode.env "/etc/obotx-rpi-control-mode.env.bak-$(date +%Y%m%d-%H%M%S)"
  fi
  sudo install -m 644 systemd/arm-test.env /etc/obotx-rpi-control-mode.env
  sudo usermod -a -G dialout obotx
fi
if [ "${1:-}" = --robot ]; then
  sudo systemctl stop obotx-rpi-control
  if [ "$#" -eq 2 ]; then
    .venv/bin/python -m app.configure_arm --with-base --max-z-difference-mm "$2" --output /home/obotx/ObotX/config/stm32-robot.json
  else
    .venv/bin/python -m app.configure_arm --robot --output /home/obotx/ObotX/config/stm32-robot.json
  fi
  if [ -f /etc/obotx-rpi-control-mode.env ]; then
    sudo cp /etc/obotx-rpi-control-mode.env "/etc/obotx-rpi-control-mode.env.bak-$(date +%Y%m%d-%H%M%S)"
  fi
  sudo install -m 644 systemd/robot.env /etc/obotx-rpi-control-mode.env
  sudo usermod -a -G dialout obotx
fi
if [ "${1:-}" = --service-test ]; then
  sudo systemctl stop obotx-rpi-control
  .venv/bin/python -m app.configure_arm --service-only --output /home/obotx/ObotX/config/stm32-arm-service.json
  .venv/bin/python -m app.service_access
  if [ -f /etc/obotx-rpi-control-mode.env ]; then
    sudo cp /etc/obotx-rpi-control-mode.env "/etc/obotx-rpi-control-mode.env.bak-$(date +%Y%m%d-%H%M%S)"
  fi
  sudo install -m 644 systemd/service-test.env /etc/obotx-rpi-control-mode.env
  sudo usermod -a -G dialout obotx
fi
if [ "${1:-}" = --auto ]; then
  sudo install -m 644 systemd/auto.env /etc/obotx-rpi-control-mode.env
  sudo usermod -a -G dialout obotx
fi
sudo install -m 644 systemd/obotx-rpi-control.service /etc/systemd/system/obotx-rpi-control.service
sudo systemctl daemon-reload
sudo systemctl enable obotx-rpi-control
sudo systemctl restart obotx-rpi-control
sleep 2
sudo systemctl is-enabled obotx-rpi-control
sudo systemctl is-active obotx-rpi-control
