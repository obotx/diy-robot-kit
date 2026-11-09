#ifndef ARM_STEP_PLAN_H
#define ARM_STEP_PLAN_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t prescaler;
    uint32_t autoreload;
    uint32_t compare;
} ArmStepPlan;

bool arm_step_plan(uint32_t timer_hz, unsigned counter_bits,
                   uint32_t frequency_millihz, ArmStepPlan *plan);
bool arm_step_plan_limits(uint32_t timer_hz, unsigned counter_bits,
                         uint32_t frequency_millihz, uint32_t max_hz,
                         uint32_t high_us, uint32_t low_us, ArmStepPlan *plan);
#endif
