# High-frequency STEP verification

The motion backend uses finite DMA blocks above 5 kHz, up to 160 kHz, on all
three axes. The UART velocity envelope for Z is
250000 um/s. This is a software capability, not a validated mechanical rating.
Existing homing speed settings and commissioning requirements remain in force.
The separate diagnostic profile described below remains available.

Motion blocks contain at most 1 ms of pulses (160 at the ceiling). The final
DMA write stops each block in hardware; a completion IRQ starts the next.
Speed changes take effect between blocks; DIR changes wait at least 1 ms.
IRQ restart latency creates small gaps: measured average speed can be lower
than the requested in-block frequency. Position is counted using DMA NDTR,
not elapsed time. Hardware path budgets and target/soft-limit budgets clip
the next block. Raw limits and aborts disable both DMA and the timer; an
ambiguous partial pulse invalidates the position through a generator fault.
The planner acceleration settings are unchanged; short moves may not reach
the requested speed, and the standalone TRAVEL endpoint remains a hard stop.

The diagnostic backend uses timer PWM and normal
(non-circular) DMA, without an interrupt for each pulse. The final DMA transfer
clears timer CEN after the final falling edge. Each command has a finite pulse
budget; STOP and DMA errors abort the train.

## Rates

For Z with 3200 pulses/revolution, a 5 mm screw lead and electronic gearing 1:1:

| Linear speed | STEP rate |
| --- | --- |
| 100 mm/s | 64 kHz |
| 200 mm/s | 128 kHz |
| 250 mm/s | 160 kHz |

These are conversions, not verified mechanical capabilities. At 200 mm/s the
motor turns at 2400 rpm. Torque, supply voltage, screw critical speed and
acceleration must be checked separately. Timer frequencies are quantized.

The pulse high time is at least 3 us and low time at least 3 us. DIR setup is
applied once per train (at least 1 ms), not once per pulse. The driver timing
diagram supplied by the user requires each pulse level to exceed 2.5 us.
Verify the waveform after the ULN as well as directly on STM: a good signal
at the MCU alone does not establish driver compatibility.

## Build and test without motors

Physically disconnect motor-driver control signals before testing. Scope
firmware holds ENA disabled, but this is not a substitute for disconnection.
Do not use this firmware for connected-actuator motion.

From the repository root in PowerShell:

```powershell
.\software\stm32_arm\tools\build-firmware.ps1 -ServiceMode -DmaScope -OutputDirectory tmp/stm32-dma-scope-build
```

Flash `tmp/stm32-dma-scope-build/obotx_arm_f446re.elf` explicitly. The ordinary
CubeIDE Debug configuration remains the standalone ZL motion build, not this
scope build. This script does not flash a board.

Open UART at 115200 baud, 8N1, newline termination. Only these commands are
accepted in this profile:

```text
SCOPE STATUS
SCOPE ZL 64000 8192
SCOPE ZL 128000 8192
SCOPE ZL 160000 8192
STOP
```

Send each train separately, with the oscilloscope armed in Single mode first.
The 128 kHz train lasts approximately 64 ms. ZR and X are also valid axis names;
only one axis can run at a time. Range: 1000..160000 Hz, 1..8192 pulses.
Read `SCOPE STATUS` afterward: state 3 is DONE; state 4 is ABORTED and 5 is
ERROR. States 0/1/2 are IDLE/SETUP/RUNNING. `ena=0` is the commanded state,
not an electrical measurement. There is no automatic repeat or resume.

| Axis | STEP pin | Timer | DMA1 update request |
| --- | --- | --- | --- |
| ZL | PA0 | TIM2 CH1 | Stream1 channel3 |
| ZR | PA1 | TIM5 CH2 | Stream0 channel6 |
| X | PA6 | TIM3 CH1 | Stream2 channel5 |

Check frequency, both level widths, exact pulse count, the final falling edge,
absence of extra pulses, and idle level. The DMA final-write latency must be
shorter than the following low interval under actual bus load. Host tests model
register transfers; they do not prove hardware timing. A stopped CPU plus a
failed DMA transfer is not covered by an independent hardware safety circuit.

Host tests cover concurrent DMA axes, counted endpoints, direction/rate changes,
DMA errors and guard/abort handling, alongside the existing motion/watchdog
tests. Hardware timing and loaded motion still require measurement. Firmware
for an isolated ZL service setup is built with:

```powershell
.\software\stm32_arm\tools\build-firmware.ps1 -ServiceMode -BenchOutputs -StandaloneZL -OutputDirectory tmp/stm32-standalone-zl-build
```

After flashing, `bench_serial.py travel --mm 20 --speed-mm-s 200
--standalone-actuator` requests 200 mm/s; retain the existing `--port COM3`
global option before `travel`. Increase speed gradually, not directly to this
value. The 20 mm distance and first-80-mm bench position restrictions are
unchanged. This short travel is not a maximum-speed qualification test.

## References

- [STM32F446 reference manual RM0390, DMA mappings and timers](https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
- [ST AN4776 timer application note](https://www.st.com/resource/en/application_note/dm00236305-pwm-generation-using-stm32-general-purpose-timers-stmicroelectronics.pdf)
