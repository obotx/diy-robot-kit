# Four-finger electric gripper

Use the **electric 4 fingers** variant of this gripper:

- [AliExpress item 1005009794922378](https://pl.aliexpress.com/item/1005009794922378.html)

Do not substitute a two-finger DC gripper.

## Electrical connection

The installed actuator is reported as an unlabelled four-wire bipolar NEMA17
stepper motor, approximately 40 mm long. Its rated phase current has not been
confirmed.

| Function | Connection |
| --- | --- |
| Driver | Pololu 2973 TB67S249FTG Full Breakout |
| Motor supply | Fused nominal 24 V motor branch; not the 12 V camera rail |
| STEP / CLK | STM32 PB8, CN10-3, TIM4_CH3 |
| DIR / CW-CCW | STM32 PB7, CN7-21 |
| ENABLE | STM32 PB9, CN10-5 |
| Logic ground | Common STM32/driver GND |
| Microsteps | 1/16: DMODE0 and DMODE1 to local VCC; DMODE2 and RESET to GND |

VCC on the Pololu board is its 5 V regulator output. Do not connect it to an
STM32 supply. LO1/LO2 use 5 V pull-ups and must not be connected directly to an
STM32 input until a suitable interface has been fitted.

## First test

1. Disconnect the gripper mechanically from any object and keep the emergency
   stop accessible.
2. Set the driver current limit to a low starting value, provisionally 0.5 A per
   phase. Do not use 2 A unless the motor rating and cooling have been verified.
3. Command the lowest available speed briefly and confirm the direction before
   assigning open/close labels.
4. Check motor and driver temperature. Increase current only when required for
   torque and within the confirmed motor/driver limits.
5. Verify the full mechanical travel without driving continuously into either
   hard stop.

There is currently no homing switch, absolute opening measurement or force
sensing. Current limiting is not object-detection or grip-force feedback. Secure
objects against falling when the driver is disabled.

Firmware behavior and limits are described in the
[gripper firmware instructions](../software/stm32_arm/docs/gripper.md).
