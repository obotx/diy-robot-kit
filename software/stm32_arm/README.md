# STM32 Arm Controller

The ARM controller uses a NUCLEO-F446RE with STM32F446RET6. It controls the
three arm axes and the four-finger gripper. The standard build communicates
with the Raspberry Pi using the JSON protocol.

For motor-disconnected STEP waveform checks, use the diagnostic procedure in
[High-frequency STEP verification](docs/high-speed-step.md).

The controller drives:

- Z-left closed-loop stepper driver,
- Z-right closed-loop stepper driver,
- X closed-loop stepper driver,
- limit inputs for each physical actuator,
- skew/racking protection for the two Z actuators,
- gripper STEP, DIR and ENABLE outputs.

## Firmware structure

The code is split into:

- `include/arm_axis_config.h` - mechanical and driver constants,
- `include/arm_debug_protocol.h` - simple PC bring-up command parser,
- `include/arm_motion.h` - portable arm motion state machine interface,
- `include/arm_nucleo_f446re_pins.h` - NUCLEO-F446RE HAL pin names,
- `src/arm_debug_protocol.c` - debug command handling for serial tests,
- `src/arm_motion.c` - portable logic with no STM32 HAL dependency,
- `docs/cubemx-f446re-config.md` - STM32CubeMX setup for the confirmed board,
- `docs/zdm-2ha865-parameters.md` - initial ZDM-2HA865 parameter sheet.

The portable core is called by the STM32 HAL/CubeMX layer. The HAL layer owns
GPIO, timers, UART/USB serial and interrupt wiring.

## Responsibilities

- receive direct actuator jog/target commands from PC or Raspberry Pi,
- run homing sequence,
- read limit switches,
- enforce soft and hard travel limits,
- synchronize the two Z drives for grouped `Z` commands,
- stop on Z skew/racking faults,
- monitor driver alarm outputs,
- report axis, homing, limit, and fault state.

## Hardware configuration

- stepper drivers use STEP/PUL, DIR, ENABLE, and ALARM signals,
- motor encoders remain connected to their closed-loop drivers,
- STM32 does not connect in parallel to motor encoder lines unless the driver
  provides a documented buffered encoder output,
- limit switches are normally-open inductive sensors,
- four inductive sensors are installed: one for each axis and one for level
  detection,
- the two Z actuators are software synchronized by the STM32.

PC12 is unused and the firmware does not detect loss of motor power. After any
motor-power interruption, reset the ARM controller and home the axes again
before commanding position moves.

## Serial commands

Independent TIM2/TIM5/TIM3 STEP generation, completed-pulse position accounting
and multi-phase homing are implemented. Physical service-mode outputs require
the deliberate `ARM_BENCH_OUTPUTS=1` build option.

The default build implements the webserver JSON link. See
[JSON actions](docs/json-actions.md) for the command contract, watchdog behavior
and commissioning requirements.

The following legacy commands require the separate `ARM_SERIAL_SERVICE_MODE=1`
build; they are NOT accepted by the JSON gateway firmware.
Commands are ASCII lines over USART2 / ST-LINK VCP at `115200 8N1`.
Positions and velocities are in micrometers and micrometers per second.

| Command | Meaning |
| --- | --- |
| `HB` or `PING` | Refresh the 1000 ms service watchdog. |
| `INPUTS` or `IO` | Raw/filtered limit inputs, alarms, POWER_OK and bench flag. |
| `STATUS` or `?` | Print current state once. |
| `EN 1` / `EN 0` | Enable or disable driver outputs. |
| `STOP` | Stop all axes. |
| `CLR` or `CLEAR` | Clear latched firmware fault. |
| `JOG Z <vel>` | Jog both Z actuators together. |
| `JOG ZL <vel>` | Jog only left Z actuator. |
| `JOG ZR <vel>` | Jog only right Z actuator. |
| `JOG X <vel>` | Jog X actuator. |
| `HOME Z` / `HOME X` | Home grouped Z or X. |
| `MOVE Z <pos> [vel]` | Move both Z actuators to an absolute position. |
| `MOVE ZL <pos> [vel]` | Move left Z actuator to an absolute position. |
| `MOVE ZR <pos> [vel]` | Move right Z actuator to an absolute position. |
| `MOVE X <pos> [vel]` | Move X actuator to an absolute position. |
| `SETLIM Z <max>` | Set runtime soft max position for both Z actuators. |
| `SETLIM ZL <max>` | Set runtime soft max position for left Z. |
| `SETLIM ZR <max>` | Set runtime soft max position for right Z. |
| `SETLIM X <max>` | Set runtime soft max position for X. |
| `SETLIM <axis> <min> <max>` | Set runtime soft min/max position. |
| `SETVEL Z <vel>` | Set default `MOVE Z` velocity when omitted. |
| `SETVEL ZL <vel>` | Set default left Z `MOVE` velocity when omitted. |
| `SETVEL ZR <vel>` | Set default right Z `MOVE` velocity when omitted. |
| `SETVEL X <vel>` | Set default X `MOVE` velocity when omitted. |
| `SETPOS Z <pos>` | Service command: set both Z software positions and mark homed. |
| `SETPOS ZL <pos>` | Service command: set left Z software position and mark homed. |
| `SETPOS ZR <pos>` | Service command: set right Z software position and mark homed. |
| `SETPOS X <pos>` | Service command: set X software position and mark homed. |

`MOVE` requires the addressed actuator to be homed. During bench tests without
limits, use `SETPOS <axis> 0` first, then send small `MOVE` commands.
Grouped `Z` remains the normal command for lifting both vertical actuators
together; `ZL` and `ZR` are intended for setup, service, and recovery.
Runtime limits and default move velocities are stored in RAM. After reset, the
firmware loads the compiled defaults from `include/arm_axis_config.h`.

## Hardware checks before first motion

- use the NUCLEO-F446RE board specified in the BOM,
- compare the STM32CubeMX pin mapping with the electrical diagram,
- test ENABLE and ALARM polarity on each ZDM-2HA865 before motion,
- test the active state of every limit switch at the STM32 input,
- measure the safe Z-left/Z-right skew limit on the assembled mechanism and set
  the firmware limit before lifting a payload.
