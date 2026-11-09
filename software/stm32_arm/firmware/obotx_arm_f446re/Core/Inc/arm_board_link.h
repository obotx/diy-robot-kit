#ifndef ARM_BOARD_LINK_H
#define ARM_BOARD_LINK_H

/* Separate builds: no automatic fallback from JSON to service commands. */
#ifndef ARM_SERIAL_SERVICE_MODE
#define ARM_SERIAL_SERVICE_MODE 0
#endif

/* Runtime interlocks and actual homing still gate every enabled command. */
#ifndef ARM_WEB_MOTION_COMMISSIONED
#define ARM_WEB_MOTION_COMMISSIONED 1
#endif
#ifndef ARM_BENCH_OUTPUTS
#define ARM_BENCH_OUTPUTS 0
#endif
#ifndef ARM_STEP_SCOPE_DMA
#define ARM_STEP_SCOPE_DMA 0
#endif
#if ARM_STEP_SCOPE_DMA && (!ARM_SERIAL_SERVICE_MODE || ARM_BENCH_OUTPUTS)
#error "DMA scope requires service mode with motor outputs disabled"
#endif
#if ARM_BENCH_OUTPUTS && !ARM_SERIAL_SERVICE_MODE
#error "Bench outputs require the service console"
#endif

void arm_board_watchdog_tick(void);
void arm_board_emergency_stop(void);
int arm_board_identity(char device_id[81], char boot_id[81]);

#endif
