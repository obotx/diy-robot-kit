# ZDM-2HA865 driver configuration

Z-left values marked as verified were confirmed on the installed
DL86H/ZDM-family controller. Apply them to another controller only after
confirming the same model, firmware and menu.

These settings assume the ZDM-2HA865 drivers are used in external pulse/direction
position mode from the STM32 arm controller.

The motor encoder cable should remain connected directly to the matching
ZDM-2HA865 driver. STM32 position is estimated from emitted STEP pulses and
validated indirectly through driver alarm/status outputs. Do not parallel the
encoder wires into STM32 unless the final driver documentation exposes a
separate buffered encoder output.

## Mechanical Mapping

| Axis | Mechanics | Travel | Motor | Selected Driver Pulses/Revolution | Steps/mm |
| --- | --- | ---: | --- | ---: | ---: |
| Z-left | KGX80-1000, configured for 5 mm/rev | 1000 mm | 4 Nm closed-loop stepper | 3200 | 640.0 |
| Z-right | KGX80-1000, configured for 5 mm/rev | 1000 mm | 4 Nm closed-loop stepper | 3200 | 640.0 |
| X | DS60-1000 with HPS60-05SW 5:1 gearbox | 1000 mm | NEMA 23 57EBP105ALC-TF0 | 3200 | 127.8 calibrated |

All three axes use 3200 pulses per motor revolution (P001=16). Z uses 5 mm per
motor revolution. X uses the DS60 module through the 5:1 gearbox; firmware uses
the measured effective value of 25.046 mm per motor revolution. This gives
640 pulses/mm for Z and approximately 127.8 pulses/mm for X.

## Front-panel programming

1. Disable motion commands and place the axis in a mechanically safe, unloaded
   condition. Keep the 24 V cutoff accessible.
2. Power the driver and wait for its standby display.
3. Hold the Enter key for about 3 seconds to enter the parameter menu.
4. Use the arrow keys to select a parameter and Enter to open it.
5. Hold Enter for about 3 seconds to edit/save a value; use the arrow keys to
   select the digit and change it.
6. Use Return to leave the menu.
7. Cycle driver power after changing the P0 group and verify every saved value
   before enabling the motor.

Do not use `P000=1111`: it resets all parameters to factory defaults. Record all
original values before making changes.

## Driver parameters

| Parameter | Z-left | Z-right | X | Action / note |
| --- | ---: | ---: | ---: | --- |
| P001 / PN001 subdivision | `16` verified | `16` | `16` | Required by current firmware: 3200 pulses per motor revolution for a 1.8 degree motor. Save, power-cycle and verify. |
| P002 / PN002 direction | keep installed value | commission | commission | Z-left direction is inverted in firmware. Do not change ZL P002 merely to reverse motion. Verify ZR/X machine-positive direction at low speed. |
| P003 / PN003 motor type | keep matched-kit value | keep matched-kit value | keep matched-kit value | Do not change. Each motor and driver was supplied as a matched kit. |
| P004 / PN004 position error limit | `4000` default | `4000` default | `4000` default | Keep the factory value for initial tests. Do not raise it to hide mechanical or encoder faults. |
| P005 / PN005 standstill current | `50%` default | `50%` default | `50%` default | Increase only after loaded holding and temperature tests. |
| P006 / PN006 gear numerator | `1` | `1` | `1` | Direct pulse mapping. |
| P007 / PN007 gear denominator | `1` | `1` | `1` | Direct pulse mapping. |
| P100 running current | `0100` verified | matched-kit value | matched-kit value | Percentage, not amperes. Use the supplied motor/driver kit value; do not copy to a different motor blindly. |
| P108 internal drive enable | `0001` verified | verify same model first | verify same model first | Required on the tested ZL unit. |
| P110 input/output level | `1000` verified | verify polarity first | verify polarity first | On tested ZL this gave holding with `EN 1` and release with `EN 0`/watchdog. Confirm ENA and alarm polarity on every unit. |
| P200 run mode | `0` verified | `0` | `0` | External position/pulse mode; `1` is internal speed mode. |

The minimum baseline for each confirmed identical Z controller is therefore
`P001=16` and `P200=0`; X also uses `P001=16` and `P200=0`. For Z-left the tested complete set is
`P001=16`, `P100=0100`, `P108=0001`, `P110=1000`, `P200=0`, with P002 left
unchanged and direction handled by firmware. Copy `P100`, `P108` or `P110` to
ZR/X only after confirming that the controller and matched motor are identical.

Do not tune current-loop, speed-loop, or position-loop parameters until the
machine can move unloaded and the default settings have been characterized. If
the driver was supplied preconfigured with the motor, record its factory values
before changing anything.

## Firmware Constants

| Axis | Max Velocity | Max Acceleration | Homing Fast | Homing Slow |
| --- | ---: | ---: | ---: | ---: |
| Z-left | 190 mm/s | 100 mm/s^2 | 2 mm/s | 0.5 mm/s |
| Z-right | 190 mm/s | 100 mm/s^2 | 2 mm/s | 0.5 mm/s |
| X | 500 mm/s | 500 mm/s^2 | 10 mm/s | 2 mm/s |

These values mirror `software/stm32_arm/include/arm_axis_config.h`. The 190 mm/s
Z ceiling is a firmware limit, not permission to start commissioning at that
speed. Begin with the low-speed bench procedure and increase only after unloaded
and loaded validation.

## Limit Switch Assumption

The current design keeps normally-open inductive limit switches. Firmware treats
an active limit input as a hard stop for motion into that limit. Because NO
sensors do not fail safe on broken wiring, the electrical system still needs a
separate normally-closed E-stop or motor-disable chain.

## Component references

- The supplied DL86H/ZDM-family manual defines P001, P002-P007, P100-P110 and
  P200 and recommends leaving the matched motor loop parameters unchanged.

- Mechanical assembly drawing: `hardware/drawings/assemblies/OBX-A009_REV.pdf`
- Horizontal actuator drawing: `hardware/drawings/assemblies/OBX-A010_REV.pdf`
- ZDM-2HA865 4 Nm kit supplier page: https://sklep.akcesoria-cnc.pl/silnik-serwo-krokowy-4nm-5-8a-serwokontroler-zdm-2ha865-p-206.html
- ZDM-2HA865 2.5 Nm kit supplier page: https://sklep.akcesoria-cnc.pl/silnik-serwo-krokowy-2-5nm-4a-serwokontroler-zdm-2ha865-cnc-p-10.html
