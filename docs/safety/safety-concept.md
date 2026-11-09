# Safety rules

- The robot must start with motion disabled.
- Keep the physical emergency stop or motor-power disconnect within reach.
- Do not use Wi-Fi, Ethernet, camera video, the browser or software STOP as an
  emergency-stop function.
- BatteryProtect protects the battery supply; it is not a certified emergency
  stop.
- Support the vertical axes whenever motor power is removed.
- Raise the drive wheels and remove payloads for initial tests.
- Commission one axis at a time at low speed.
- Stop motion on communication loss, watchdog timeout, driver alarm or active
  limit switch.
- Do not bypass fuses, limit switches, driver alarms or interlocks.
- Check cable, fuse, connector, motor and driver temperatures after initial
  tests and after changing the load or current settings.

## Before every powered test

1. Inspect the wiring and mechanical fasteners.
2. Confirm that the correct fuses are fitted and the main disconnect works.
3. Confirm that the emergency-stop or motor-disable path removes motor torque.
4. Confirm that the logic controllers report the disabled or fault state.
5. Verify limit-switch operation by hand before commanding an axis toward it.
6. Keep people clear of the robot's motion envelope.

The low-level controllers must reject malformed or out-of-range commands and
stop motion when enable is false, the watchdog expires, an alarm is active or a
hard limit is reached in the commanded direction.
