# ObotX Raspberry Pi control server

The Raspberry Pi application provides the browser operator panel, communicates
with the ARM and BASE STM32 controllers, displays four camera previews and reads
SmartShunt telemetry.

## Install

Follow [INSTALL.md](INSTALL.md). Then configure:

- [Wi-Fi, phone hotspot, direct Ethernet and SSH](docs/network-connection.md),
- [cameras](docs/cameras.md),
- [SmartShunt Bluetooth telemetry](docs/smartshunt.md),
- [STM32 communication](docs/stm32-link.md).

Open the panel at:

```text
http://rpi-control.local:8000/
```

If `.local` name resolution is unavailable, replace `rpi-control.local` with
the Raspberry Pi address shown by `hostname -I` or by the network guide.

## Service control

```bash
systemctl is-enabled obotx-rpi-control
systemctl is-active obotx-rpi-control
journalctl -u obotx-rpi-control -n 50 --no-pager
sudo systemctl restart obotx-rpi-control
```

The panel is intended for a trusted robot LAN. Do not expose port 8000 to the
Internet. The physical emergency stop remains the primary way to stop dangerous
motion.

## Update

Stop the robot and preserve `/home/obotx/.config/obotx/`. After updating the
repository:

```bash
cd /home/obotx/ObotX/software/rpi
.venv/bin/python -m pip install -r requirements.txt
sudo systemctl restart obotx-rpi-control
```

Repeat homing and commissioning checks after firmware, wiring or mechanical
changes.

## Software checks

```bash
.venv/bin/python -m unittest discover -s tests -p "test_*.py"
```

Software tests do not verify wiring, motor direction, stopping distance,
payload capacity or physical safety functions.
