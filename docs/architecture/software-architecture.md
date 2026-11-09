# Software Architecture

## Raspberry Pi

The Python service:

- serves the operator panel and robot API,
- accepts one active motion owner,
- validates command sequence, lease and watchdog timing,
- communicates with ARM and BASE over separate USB serial links,
- merges controller state into robot telemetry,
- provides password-protected service control,
- processes RTSP camera streams for browser previews,
- reads SmartShunt Bluetooth Instant Readout data,
- stops commanded motion when control ownership or communication is lost.

## STM32 BASE

The BASE firmware:

- receives complete base velocity setpoints,
- applies mecanum kinematics and acceleration ramps,
- regulates wheel speed from four quadrature encoders,
- generates PWM/DIR signals for two MDD10A drivers,
- enforces command and timing limits,
- disables outputs and reports latched faults after invalid commands, feedback
  errors or watchdog expiry.

## STM32 ARM

The ARM firmware:

- controls Z-left, Z-right and X STEP/DIR/ENABLE outputs,
- controls the four-finger stepper gripper,
- performs guided referencing and axis homing,
- enforces hard inputs, software travel limits and Z-axis skew limits,
- stops outputs and reports latched faults after invalid commands, driver alarms
  or watchdog expiry.

## Configuration

Robot-specific configuration includes controller identities, serial ports,
direction signs, encoder scaling, geometry, speed and acceleration limits, axis
travel, switch polarity, service password, cameras and SmartShunt credentials.
Secrets are stored locally on the Raspberry Pi and must not be committed.
