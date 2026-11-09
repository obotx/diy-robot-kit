# ObotX Control API v1

The STM32 gateway and simulation backend use the same commands. See
`../docs/stm32-link.md` for the serial-controller connection. The
`/api/outputs/*` relay endpoints are separate and are not used by the operator
panel.

## Connection

- Control and telemetry: `ws://rpi-control.local:8000/ws/control`.
- Read-only snapshot: `GET http://rpi-control.local:8000/api/robot`.
- One JSON object per WebSocket text message, not newline-delimited batches.
- Run one Uvicorn worker. Robot state and ownership are in process memory.
- Browser Origin must match the server. Native clients may omit Origin.
- The control API has no authentication: use it only on a trusted local network. Client
  ownership prevents conflicting commands; it is not access control.
- Cameras use separate HTTP streams, never the control WebSocket.

The server first sends:

```json
{"type":"welcome","protocol":1,"client_id":"server-generated-id","watchdog_ms":600}
```

It then sends `telemetry` approximately every 100 ms, including to observers.
All command messages must have a strictly increasing integer `seq` within
the current connection, from 0 to 9007199254740991. Reset numbering on a new
connection. Unknown fields, missing fields, invalid types, nonfinite numbers,
out-of-range values and reused sequence numbers are rejected.

## Claim and Release

```json
{"type":"enable","seq":1,"enabled":true,"source":"my-pc-app"}
```

Only one connection can own control. Another claim returns `busy`; an observer
may continue reading telemetry. `source` is a display name of 1-40 characters.
The server replies to successful commands:

```json
{"type":"ack","seq":1}
```

This ACK means gateway acceptance, not a completed physical action or a device
ACK. In serial modes `outputs_enabled` reflects acknowledged device state.
Additional telemetry fields are `control_ready`, `control_blocked_reason` and
`controllers` (link status, identities and latest device replies). Hardware
position fields can be null; never treat them as zero. `backend` is `mock`,
`stm32` or `stm32-emulator`; `hardware_connected` is false for the emulator.

Release your claim:

```json
{"type":"enable","seq":2,"enabled":false}
```

## Motion

Optional `acceleration` on a motion message defaults to:

```json
{"base_mps2":0.3,"rotation_radps2":0.6,"z_mps2":0.1,"x_mps2":0.2,"gripper_per_s":2.0}
```

Bounds: base 0.01-1 m/s2, rotation 0.05-2 rad/s2, Z 0.005-0.5 m/s2,
X 0.01-1 m/s2, gripper 0.02-4 normalized velocity units per second. The gripper
simulator maps normalized speed 1 to 50 %/s. Values are software test bounds,
not a claim of mechanically safe operation. Firmware must enforce calibrated
limits. An omitted object uses defaults, not the previous client's settings.
Simulation applies ramps to both acceleration and ordinary deceleration; STOP
and disable clear simulated velocity immediately. `command` remains the target;
`simulated_velocity` reports the simulator's ramped velocity, not hardware data.
Grouped Z appears as equal `zl_mps`/`zr_mps` in simulated velocity.

Send a complete motion state every 100 ms while enabled, even when all values
are zero. Releasing a key must immediately send zero for that motion component.
Do not use a background heartbeat that replays a stale nonzero command after
the operator input source has failed.

```json
{"type":"motion","seq":3,"vx_mps":0.1,"vy_mps":0.0,"wz_radps":0.0,"z_mps":0.0,"zl_mps":0.0,"zr_mps":0.0,"x_mps":0.0,"gripper":0.0}
```

| Field | Positive direction | Limit |
| --- | --- | --- |
| `vx_mps` | Forward relative to platform | +/- 0.3 m/s |
| `vy_mps` | Left relative to platform | +/- 0.3 m/s |
| `wz_radps` | Counterclockwise viewed from above | +/- 0.6 rad/s |
| `z_mps` | Raise Z-left and Z-right together | +/- 0.05 m/s |
| `zl_mps` | Raise Z-left independently | +/- 0.05 m/s |
| `zr_mps` | Raise Z-right independently | +/- 0.05 m/s |
| `x_mps` | Extend horizontal slide | +/- 0.1 m/s |
| `gripper` | Open; negative closes | -1 to +1 |

The magnitude `sqrt(vx_mps^2 + vy_mps^2)` must also be <= 0.3 m/s.
Four independent browser speed percentages multiply the limits for the base,
Z axes, X rail and gripper. Base translation is normalized for diagonal input.
These are simulator limits, not validated hardware settings.

`zl_mps` and `zr_mps` default to zero when omitted, retaining compatibility with
0.2.0 clients. Group Z and individual Z commands must not be nonzero in the same
message: this returns `conflicting_z_commands` and stops the owner. The browser
gives held group-Z keys priority over independent Z keys.

Group Z applies the same displacement to both supports, preserving their
existing height difference. If either support reaches a travel limit, group
motion stops at that limit for both. Independent commands move only their
respective support; both individual commands may be used together.
There is no angle-target command. The drawing derives a schematic rail tilt
from the two heights; it is not a calibrated angle or a mechanical model.
The entire X rail translates through the mounting carriages, with the gripper
fixed to its end. Existing STM32 skew protection must be reconciled with the
intended tilting mechanism before hardware integration; no firmware change
is included in this simulator update.

## Stop and Timeout

Any connected client, including an observer, may send:

```json
{"type":"stop","seq":4}
```

STOP zeros all motion and releases ownership. Owner disconnect, malformed
owner command, or no accepted motion for 600 ms does the same. Repeated enable
messages do not refresh a motion deadline. The watchdog is checked on a 20 ms
server loop and when state/commands are accessed; this is not a hard real-time
guarantee. A fresh explicit enable is required after a stop or reconnect.

This software STOP is not a hardware emergency stop. Future STM32 integration
must enforce its own watchdog, limits, homing and hardware disable chain.

## Telemetry

`GET /api/robot` and WebSocket `telemetry` share the same schema:

```json
{
  "type":"telemetry","protocol":1,"backend":"mock",
  "enabled":false,"owner":null,"source":null,"reason":"startup",
  "watchdog_ms":600,"command_age_ms":null,
  "command":{"vx_mps":0.0,"vy_mps":0.0,"wz_radps":0.0,"z_mps":0.0,"zl_mps":0.0,"zr_mps":0.0,"x_mps":0.0,"gripper":0.0},
  "base":{"x_m":0.0,"y_m":0.0,"yaw_rad":0.0},
  "arm":{"z_left_mm":350.0,"z_right_mm":350.0,"x_mm":300.0,"travel_mm":1000,"homed":false},
  "gripper":{"opening_pct":70.0},"hardware_connected":false
}
```

Positions are integrated simulated values, not measured telemetry. Z and X
are clamped to 0-1000 mm; gripper to 0-100%. The gripper simulation moves at
50 percentage points/second at full command. No wheel RPM, battery voltage or
homing success is fabricated. Wheel highlights indicate a base motion command,
not measured individual wheel rotation. All state resets on server restart.

Errors use `{"type":"error","code":"..."}`. Codes include `busy`,
`not_owner`, `invalid_command`, `stale_sequence`, `base_speed_limit`, `conflicting_z_commands`,
and `message_too_large`. Application messages are limited to 4096 characters.

## PC Example

`examples/pc_client.py` is an independent native Python client. It only reads
telemetry by default. Install `websockets==17.1` in your PC Python environment:

```powershell
python -m pip install websockets==17.1
python examples/pc_client.py --url ws://rpi-control.local:8000/ws/control
```

Add `--demo` to explicitly claim control, jog the grouped Z axes for one second
in confirmed mock mode, and stop. Release browser control first. The same JSON
protocol can be used from a C#, Python, C++ or other local application.

## Reference

Implementation follows the existing project gateway architecture and uses
[FastAPI WebSockets](https://fastapi.tiangolo.com/advanced/websockets/) and the
[browser WebSocket API](https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API).
