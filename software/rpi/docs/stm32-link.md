# Raspberry Pi to STM32 connection

ObotX uses two NUCLEO-F446RE boards connected to the Raspberry Pi by USB:

- ARM controller for Z, X and gripper motion,
- BASE controller for the four mecanum wheels.

Both boards use the JSON v1 serial protocol at 115200 baud. The server assigns
roles from the controller identity response, not from USB connection order.

## Install firmware

Build and flash the boards using their instructions:

- [ARM firmware](../../stm32_arm/README.md),
- [BASE firmware](../../stm32_base/README.md).

Keep motor power disabled while flashing or identifying boards.

## Install the Raspberry Pi service

Connect both Nucleo boards by USB and run:

```bash
cd /home/obotx/ObotX/software/rpi
sh systemd/install.sh --robot
```

The installer records stable controller identities and enables the service.
Verify it after installation and after every USB wiring change:

```bash
systemctl is-active obotx-rpi-control
journalctl -u obotx-rpi-control -n 80 --no-pager
ls -l /dev/serial/by-id/
```

The panel must show one ARM and one BASE controller. Do not assign roles by
`/dev/ttyACM0` and `/dev/ttyACM1`, because these numbers can swap after reboot.

## First connection check

1. Raise the drive wheels and remove payloads.
2. Keep motion disabled in the panel.
3. Confirm both controllers are online without faults.
4. Check ARM limit switches, driver alarms and homing at low speed.
5. Check BASE wheel direction and encoder feedback one wheel at a time.
6. Use the physical emergency stop during every motion test.

If a controller is missing, stop the service and check USB devices:

```bash
sudo systemctl stop obotx-rpi-control
lsusb
ls -l /dev/serial/by-id/
sudo systemctl start obotx-rpi-control
```

Only one program may open a controller serial port at a time. Close serial
terminals before starting the ObotX service.

## Communication safety

The controller watchdog stops commanded motion when valid messages stop. A
browser STOP, serial watchdog or software fault is not a substitute for the
physical emergency-stop or motor-power disconnect.
