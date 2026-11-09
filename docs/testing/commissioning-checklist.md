# Commissioning Checklist

Use this checklist during initial commissioning. Do not skip safety checks.

## Before Power-Up

- [ ] Mechanical fasteners checked.
- [ ] Battery secured low in the base.
- [ ] Wheels/drive modules mounted correctly.
- [ ] Arm axis modules mounted and aligned.
- [ ] Moving cables have strain relief.
- [ ] No loose tools or metal parts inside electronics area.
- [ ] Fuse rating selected and installed.
- [ ] Main disconnect installed.
- [ ] E-stop or motor disable verified electrically.
- [ ] Polarity checked before connecting electronics.
- [ ] DC/DC outputs checked without controllers connected.

## Controller Bring-Up

- [ ] Raspberry Pi boots reliably.
- [ ] STM32 base controller powers correctly.
- [ ] STM32 arm controller powers correctly.
- [ ] Serial links detected.
- [ ] Controller firmware version is logged.
- [ ] Robot starts disabled.
- [ ] Enable command changes state without motion.
- [ ] Disable command stops all motion.
- [ ] Watchdog timeout stops motion.

## Base Test

- [ ] Test with wheels unloaded or robot constrained.
- [ ] Motor directions verified one at a time.
- [ ] Forward/backward command verified.
- [ ] Sideways command verified.
- [ ] Rotation command verified.
- [ ] Stop command verified.
- [ ] Reduced speed limits used during first tests.

## Arm Test

- [ ] Limit switches read correctly.
- [ ] Limit switch polarity verified.
- [ ] Homing direction verified at low speed.
- [ ] Homing sequence verified.
- [ ] Soft limits configured.
- [ ] Vertical axis cannot fall freely on power loss.
- [ ] Horizontal extension tested without payload.
- [ ] Payload test starts below 3 kg and increases gradually.

## Gripper Test

- [ ] Open command verified.
- [ ] Close command verified.
- [ ] Mechanical interference checked.
- [ ] Gripper mount inspected after repeated cycles.

## Demonstration Readiness

- [ ] Battery voltage monitored.
- [ ] No active faults.
- [ ] Telemetry visible externally.
- [ ] E-stop/motor disable within reach.
- [ ] Test area is clear.
- [ ] Demo payload within confirmed safe range.

