# STM32CubeMX Configuration - NUCLEO-F446RE Arm Controller

Use this configuration for the NUCLEO-F446RE arm controller.

Board: NUCLEO-F446RE / STM32F446RET6.

## Project

Create a new STM32CubeIDE project from board selector:

- board: `NUCLEO-F446RE`,
- firmware package: STM32CubeF4,
- toolchain: STM32CubeIDE,
- timebase: `TIM6` if CubeMX asks to avoid SysTick conflicts.

Keep SWD enabled:

- `PA13` = SWDIO,
- `PA14` = SWCLK.

## Clock

Use a conservative first setup:

- HSI or ST-LINK MCO/HSE are both acceptable for bring-up,
- target system clock: `84 MHz` or `180 MHz`,
- APB timer clocks must be checked before calculating STEP timer periods.

The firmware should calculate timer reload values from the real HAL clock values,
not from hardcoded assumptions.

## USART2 - PC Debug Serial

Use the Nucleo ST-LINK virtual COM port:

| Signal | Pin | Mode |
| --- | --- | --- |
| USART2_TX | PA2 | Alternate Function |
| USART2_RX | PA3 | Alternate Function |

Serial settings:

- `115200`,
- `8N1`,
- no hardware flow control,
- receive interrupt or DMA receive-to-idle.

## STEP Timers

Initial dry-run mapping:

| Axis | STEP Pin | Timer |
| --- | --- | --- |
| Z-left | PA0 | TIM2_CH1 |
| Z-right | PA1 | TIM2_CH2 |
| X | PA6 | TIM3_CH1 |

TIM2 channels share one timer frequency. This is acceptable for basic
synchronized Z motion and homing because the two Z axes normally move at the
same speed and either channel can be disabled when its home switch latches. If
active skew correction later needs different Z-left and Z-right frequencies,
move one Z STEP output to an independent timer or use an interrupt/DMA pulse
scheduler.

For early testing, configure STEP pins as timer PWM outputs with 50% duty. The
application layer should set the timer frequency from
`step_frequency_millihz`.

## GPIO Outputs

| Signal | Pin | Initial State |
| --- | --- | --- |
| Z-left DIR | PB0 | low |
| Z-left ENABLE | PB1 | disabled |
| Z-right DIR | PC1 | low |
| Z-right ENABLE | PC2 | disabled |
| X DIR | PA7 | low |
| X ENABLE | PB10 | disabled |
| Z-left brake release | PB12 | brake engaged |
| Z-right brake release | PB13 | brake engaged |
| Gripper DIR | PB7 | low |

Do not connect ENABLE or brake outputs directly to power devices without the
proper interface stage.

## GPIO Inputs

| Signal | Pin | Firmware input |
| --- | --- | --- |
| Z-left ALARM | PC0 | active after polarity confirmation |
| Z-right ALARM | PC3 | active after polarity confirmation |
| X ALARM | PC4 | active after polarity confirmation |
| Z-left lower limit | PC5 | active when NO sensor detects target |
| Z-left upper limit | PC6 | active when NO sensor detects target |
| Z-right lower limit | PC7 | active when NO sensor detects target |
| Z-right upper limit | PC8 | active when NO sensor detects target |
| X retracted limit | PC9 | active when NO sensor detects target |
| X extended limit | PA8 | active when NO sensor detects target |
| Spare/gripper open | PA9 | raw telemetry |
| Spare/gripper closed | PA10 | raw telemetry |

PC12 is unused. The controller does not measure motor-power presence.

The controller exposes six axis-limit inputs, but the robot uses four inductive
sensors: one for each axis and one for level detection. Connect them according
to the electrical diagram and keep unused inputs in their inactive state.

Because the limit sensors are 6-36 V industrial sensors, they must go through a
24 V input interface/opto stage. Do not wire sensor output directly into STM32.

## ADC

| Signal | Pin | ADC |
| --- | --- | --- |
| Gripper current sense | PA4 | ADC1_IN4 |

This can be left unimplemented during the first stepper-only bring-up.

## Project integration

After CubeMX generates the project:

1. Copy or include `software/stm32_arm/include/*.h` into the CubeIDE include path.
2. Add `software/stm32_arm/src/arm_motion.c` to the build.
3. Add a small HAL adapter that:
   - reads GPIO inputs into `ArmMotionInputs`,
   - calls `arm_motion_tick()` every 1 ms or 10 ms,
   - applies `ArmMotionOutputs` to DIR/ENABLE GPIO and STEP timers,
   - keeps software position in sync with generated motion.
4. Start with motor power disconnected and verify telemetry over USART2.

## Terminal test

Open the ST-LINK COM port on the PC at `115200 8N1`.

Before connecting drivers, the firmware should accept:

```text
HB
EN 1
SETPOS Z 0
SETPOS X 0
JOG Z 10000
JOG Z 0
JOG X 20000
MOVE X 10000 20000
STOP
EN 0
```

The production Raspberry Pi protocol can stay JSON-based. The simple text
commands above are only for direct PC bring-up.
