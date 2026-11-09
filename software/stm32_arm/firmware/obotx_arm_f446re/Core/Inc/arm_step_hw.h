#ifndef ARM_STEP_HW_H
#define ARM_STEP_HW_H
#include "arm_motion.h"
void arm_step_hw_init(void);
bool arm_step_hw_apply(const ArmMotionOutputs *outputs);
void arm_step_hw_abort(void);
void arm_step_hw_guard(void);
bool arm_step_hw_begin_limit(ArmAxisId axis, uint32_t pulses, bool positive);
bool arm_step_hw_begin_home_limit(void);
bool arm_step_hw_limit_done(ArmAxisId axis);
void arm_step_hw_feedback(ArmMotionState *state);
void arm_step_hw_irq(ArmAxisId axis);
void arm_step_hw_dma_irq(ArmAxisId axis);
bool arm_board_pulse_allowed(ArmAxisId axis, bool positive);
#endif
