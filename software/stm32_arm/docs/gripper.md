# Pololu 2973 gripper

Electric 4 fingers, bipolar NEMA17. No homing switch, homing operation,
absolute opening percentage or automatic object/force detection.

Existing axis wiring unchanged. New outputs: PB8/CN10-3 -> CLK (TIM4_CH3),
PB7/CN7-21 -> CW/CCW, PB9/CN10-5 -> ENABLE, common GND. No ULN.
VIN is fed from the fused motor battery branch (nominal 24 V, about 29 V charged),
not the camera 12 V converter. VCC is the driver's own 5 V output, never tied to STM supplies.
DMODE0/1 -> local VCC, DMODE2/RESET -> GND gives 1/16 microsteps.

JSON full-state setpoint command.gripper ranges from -1 to +1. Magnitude maps
to at most 16000 microsteps/s (trial ceiling of 300 rpm assuming 1.8-degree
motor and 1/16 microsteps, not a validated motor maximum). Start at the lowest
slider setting after updating: the existing slider now commands 16 times the
previous speed. Acceleration scales by the same factor, preserving ramp time.
Direction sign must be checked unloaded before
calling it open/close. acceleration.gripper_per_s controls the ramp.
Zero stops pulses immediately; ENABLE is released after 1 second of idle time.
Repeated zero commands do not extend this delay. A new nonzero command resumes
motion. Disabled setpoints, faults and the communication watchdog release ENABLE
immediately, without this delay. Mechanical self-locking must retain the object;
there is no powered holding force after ENABLE is released.
Position telemetry remains null. Normal arm commissioning/homing prerequisites
still apply; this adds no gripper homing. Service setpoints support the gripper
when the controller advertises service_gripper_v1.

There is no fixed continuous-motion duration limit. Hold-to-run commands can
continue while the operator keeps the button pressed and commands keep arriving.
The existing 250 ms communication watchdog and arm emergency stop also disable
the gripper. No fault inputs LO1/LO2 have been connected/implemented yet.

Set current manually before powering motion: motor rating is unknown, 2 A is
unverified; start low (proposed 0.5 A/phase unloaded). Current limiting is
NOT force sensing or collision protection. Do not clamp
hard stops to establish a zero. Secure objects against falling when disabled.
Firmware is compiled but physical waveform/direction/temperature tests remain.

TIM4 uses a 2 MHz timebase, giving 31 us high and 31.5 us low at 16 kHz.
The driver requires at least 0.5 us for each CLK level:
https://www.pololu.com/file/0J1523/TB67S249FTG_datasheet_en_20170818.pdf
