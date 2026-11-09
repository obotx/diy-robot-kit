# STM32 BASE firmware

The BASE controller runs on a NUCLEO-F446RE and controls four mecanum-wheel
motors through two Cytron MDD10A drivers. It uses closed-loop wheel-speed
control from four quadrature encoders.

Wheel order is front left, front right, rear left, rear right. Positive robot
axes are forward X, left Y and counter-clockwise yaw.

## Configured drivetrain

The values in `include/base_drive_config.h` are:

| Parameter | Value |
| --- | ---: |
| Wheel diameter | 0.1524 m |
| Wheelbase | 0.600 m |
| Track | 0.500 m |
| Encoder counts per wheel revolution | 3450 |
| Wheel-speed command ceiling | 0.6 m/s |
| PWM ceiling | 90% |
| PI proportional gain | 0.4 |
| PI integral gain | 1.0 |

The controller regulates wheel speed; it does not publish base odometry.

## Build

In STM32CubeIDE import
`firmware/obotx_base_f446re` using **File > Import > General > Existing Projects
into Workspace**. Leave **Copy projects into workspace** disabled, then build
the Debug configuration.

The output is:

```text
firmware/obotx_base_f446re/Debug/obotx_base_f446re.elf
```

Select the BASE board by its ST-LINK serial number in the debug configuration.
The BASE configuration uses GDB port 61244 and SWV port 61245 so that it can be
used alongside the ARM configuration. Flash the ELF and press Resume.

Do not mass-erase the controller: flash sector 7 contains the boot counter used
by the communication protocol.

## Operation

USART2 through ST-LINK VCP uses 115200 baud, 8 data bits, no parity and one stop
bit. The JSON protocol requires a handshake and complete velocity setpoints.
The 250 ms command watchdog stops PWM if valid commands cease. Malformed or
replayed commands invalidate the session and stop the wheels.

The firmware uses 20 kHz PWM, direction outputs, a 10 ms control interval and
acceleration ramps. Encoder-loss and implausible-speed checks latch a fault.
External 10 kOhm PWM-to-ground resistors keep the MDD10A inputs off during
reset. The MDD10A has no separate enable input.

## First motion check

1. Raise all wheels and keep the physical emergency stop accessible.
2. Verify each wheel's positive direction and encoder sign individually.
3. Command low speed and confirm measured speed has the commanded sign.
4. Test STOP, communication loss and controller reset.
5. Place the robot on the floor only after all four wheel tests pass.

The PWM ceiling is not a current limit. Monitor supply current, connectors,
drivers and motor temperature during loaded tests.

## Tests

```powershell
.\tools\build-firmware.ps1
.\tools\test-drive.ps1
.\tools\test-link.ps1
```

These checks build the firmware and verify the regulator and serial protocol.
They do not verify physical wiring or stopping distance.

Use the [complete electrical diagram](../../hardware/wiring/ObotX_electrical_diagram.pdf)
for controller, MDD10A, motor and encoder connections.
