# Raspberry Pi installation

The Raspberry Pi runs the browser panel, communicates with the ARM and BASE
controllers, displays camera previews and reads SmartShunt telemetry.

## Requirements

- Raspberry Pi OS and user `obotx`,
- repository at `/home/obotx/ObotX`,
- both NUCLEO-F446RE controllers connected by USB for robot mode,
- motor power disabled during installation.

## Installation options

- **Repository installation:** follow the commands below when the complete
  repository is already present on the Raspberry Pi.
- **Hotspot package:** use the prepared
  [`ObotX-RPi-hotspot.zip`](packages/ObotX-RPi-hotspot.zip) to install the same
  control server and configure the Raspberry Pi as a standalone Wi-Fi access
  point. Follow the [hotspot installation guide](docs/hotspot-installation.md).

The hotspot ZIP is an installation package, not a bootable SD-card image. Start
with Raspberry Pi OS Lite (64-bit) and do not write the ZIP directly to a card.

## Repository installation

```bash
cd /home/obotx/ObotX/software/rpi
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
sh systemd/install.sh --robot
```

The installer records both controller identities, installs the systemd service
and enables it at boot. It does not start motion or homing.

```bash
systemctl is-enabled obotx-rpi-control
systemctl is-active obotx-rpi-control
journalctl -u obotx-rpi-control -n 50 --no-pager
```

Open `http://<RPi-address>:8000/`. Use the
[network guide](docs/network-connection.md) to find or configure the address.

## SmartShunt

Configure the SmartShunt in VictronConnect, then run:

```bash
sh systemd/install-battery.sh
```

Enter the Instant Readout MAC address and encryption key only in the local
prompt. Never store the key in Git, screenshots, chat or shell arguments. See
[SmartShunt setup](docs/smartshunt.md).

## Cameras

Configure camera addresses and credentials in the operator panel. They are
stored outside Git in `/home/obotx/.config/obotx/cameras.json`. The installed
robot uses `eth0` for its dedicated camera network; do not replace that profile
with ordinary DHCP. See [camera configuration](docs/cameras.md).

## Update

Stop the robot and preserve `/home/obotx/.config/obotx/` and the STM32
configuration. After updating the repository:

```bash
cd /home/obotx/ObotX/software/rpi
.venv/bin/python -m pip install -r requirements.txt
sudo systemctl restart obotx-rpi-control
```

Repeat homing and commissioning checks after firmware or mechanical changes.

## Software checks

```bash
.venv/bin/python -m unittest discover -s tests -p "test_*.py"
```

Software tests do not confirm physical wiring, motor direction, stopping
distance, payload or safe operating limits.
