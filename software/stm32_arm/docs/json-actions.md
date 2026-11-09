# ARM JSON: implemented action extension

Firmware `arm-link-2`, protocol envelope version 1, capability `arm_actions_v1`.
115200 8N1, one ASCII JSON object per LF-terminated line, maximum 2048 bytes.
Not interchangeable with the ASCII service console.

## Handshake

```json
{"type":"hello","protocol":1,"session":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":1}
```

Use a new random 32-character lowercase hexadecimal session in a real client.
Identity returns MCU ID, boot ID and capabilities. Subsequent frames must use
that session and strictly increasing integer seq values. No replay/retry of a
timed-out action: stop/reconnect, inspect state and request a new action explicitly.

## Actions

Each object below also requires protocol/session/seq as in the handshake.

| type | action | Additional required fields |
| --- | --- | --- |
| arm_action | status | None; does not refresh watchdog |
| arm_action | keepalive | None; refreshes watchdog without restarting an action |
| arm_action | stop | None; stops and disables |
| arm_action | clear_fault | None; disabled/idle, power and inputs checked |
| arm_action | home | axis: Z or X on full firmware; ZL on zl_only firmware |
| arm_action | move | axis: ZL/ZR/X/Z, position_um, velocity_um_s |
| arm_action | configure | axis, min_um, max_um, velocity_um_s |

Example absolute move, after completed homing:

```json
{"type":"arm_action","protocol":1,"session":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":2,"action":"move","axis":"ZL","position_um":20000,"velocity_um_s":10000}
```

Actions home/move/configure require disabled idle state. HOME/MOVE enable the
selected operation and disable after completion. Send keepalive about every
80-100 ms while active. No action starts implicitly on keepalive. Concurrent
enabled velocity setpoints are rejected; a disabled setpoint cancels the action.
Malformed/invalid/replayed commands stop and invalidate the session.

Positions/limits: integer um, 0..1000000, within existing axis soft limits.
Speed: positive integer um/s, Z up to 190000, X up to 500000. STEP limit 160 kHz.
MOVE also accepts optional integer `acceleration_um_s2`: Z 5000..100000,
X 10000..500000. It applies atomically to the selected axis (both axes for Z).
Omission preserves the stored acceleration. The operator panel sends the
selected Z/X acceleration explicitly.
Configure applies limits and default MOVE speed atomically in RAM, without
claiming homing or writing Flash. Both MIN and MAX must be supplied.

## Telemetry

Reply type state retains the original envelope and accepted/enabled/ready/fault.
The arm object adds mode (0 idle, 1 jog, 2 move, 3 home), active_axis (or null),
homed_axes (ZL/ZR/X booleans), and fault_code. Coordinates remain millimetres;
unreferenced axes are null. In zl_only, aggregate homed/ready refer to ZL alone;
ZR/X stay explicitly unhomed and inactive. In full-arm firmware all three axes
must be homed for enabled velocity commands. Accepted is not motion completion.

The velocity setpoint schema remains unchanged and uses m/s and m/s2. New
firmware accepts Z +/-0.19 m/s and X +/-0.5 m/s, with existing acceleration and
travel/skew guards. The operator UI limits jogging to 0.2 m/s. The 250 ms MCU
watchdog, input faults and counted STEP feedback remain active.

## Builds and verification

```powershell
.\software\stm32_arm\tools\build-firmware.ps1 -JsonZl -OutputDirectory tmp/stm32-json-zl-build
.\software\stm32_arm\tools\build-firmware.ps1
.\software\stm32_arm\tools\test-link.ps1
.\software\stm32_arm\tools\test-motion.ps1
```

First build: isolated ZL only, real motion permitted after real homing/interlocks.
Second: full-arm JSON. Neither flashes hardware. Tests exercise compiled portable
C with a synthetic plant and the actual Python SerialPeer/gateway, not actual
motors. Scope/loaded-motion verification remains a physical commissioning step.

See [Raspberry Pi installation](../../rpi/INSTALL.md) and
[STM32 connection](../../rpi/docs/stm32-link.md).
