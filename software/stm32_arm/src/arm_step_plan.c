#include "arm_step_plan.h"
#include "arm_axis_config.h"

bool arm_step_plan(uint32_t timer_hz, unsigned counter_bits,
                   uint32_t frequency_millihz, ArmStepPlan *plan)
{
    return arm_step_plan_limits(timer_hz, counter_bits, frequency_millihz,
                               ARM_STEP_MAX_HZ,
                               frequency_millihz > ARM_STEP_OPM_MAX_HZ * 1000U ? 3U : ARM_STEP_HIGH_US,
                               frequency_millihz > ARM_STEP_OPM_MAX_HZ * 1000U ? 3U : ARM_STEP_SETUP_US, plan);
}

bool arm_step_plan_limits(uint32_t timer_hz, unsigned counter_bits,
                         uint32_t frequency_millihz, uint32_t max_hz,
                         uint32_t high_us, uint32_t low_us, ArmStepPlan *plan)
{
    if (!timer_hz || !plan || (counter_bits != 16 && counter_bits != 32) ||
        !frequency_millihz || !high_us || !low_us ||
        (uint64_t)frequency_millihz > (uint64_t)max_hz * 1000U) return false;
    uint64_t clocks = ((uint64_t)timer_hz * 1000U + frequency_millihz / 2U) / frequency_millihz;
    uint64_t counts_max = UINT64_C(1) << counter_bits;
    uint64_t divisor = (clocks + counts_max - 1U) / counts_max;
    if (!divisor) divisor = 1;
    if (divisor > 65536U) return false;
    uint64_t period = (clocks + divisor / 2U) / divisor;
    uint64_t high = ((uint64_t)timer_hz * high_us + divisor * 1000000U - 1U) /
        (divisor * 1000000U);
    uint64_t setup = ((uint64_t)timer_hz * low_us + divisor * 1000000U - 1U) /
        (divisor * 1000000U);
    if (period > counts_max || period < high + setup || !high) return false;
    plan->prescaler = (uint32_t)(divisor - 1U);
    plan->autoreload = (uint32_t)(period - 1U);
    /* PWM2: low first, high at the end, low again at the one-pulse update. */
    plan->compare = (uint32_t)(period - high);
    return true;
}
