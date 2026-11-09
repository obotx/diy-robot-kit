#ifndef ARM_STEP_BURST_H
#define ARM_STEP_BURST_H
#include "arm_axis_config.h"
#include <stdbool.h>
#include <stdint.h>
#define ARM_BURST_MAX_HZ 160000U
#define ARM_BURST_MAX_PULSES 8192U
typedef enum { ARM_BURST_IDLE, ARM_BURST_SETUP, ARM_BURST_RUNNING,
               ARM_BURST_DONE, ARM_BURST_ABORTED, ARM_BURST_ERROR } ArmBurstState;
typedef struct {
    ArmBurstState state;
    ArmAxisId axis;
    uint32_t hz, pulses;
} ArmBurstStatus;
void arm_step_burst_init(void);
bool arm_step_burst_start(ArmAxisId axis, uint32_t hz, uint32_t pulses);
void arm_step_burst_poll(void);
void arm_step_burst_abort(void);
ArmBurstStatus arm_step_burst_status(void);
#endif
