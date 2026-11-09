#ifndef ARM_GRIPPER_H
#define ARM_GRIPPER_H
#include <stdbool.h>
#include <stdint.h>
void arm_gripper_init(void);
void arm_gripper_stop(void);
bool arm_gripper_guard(void);
void arm_gripper_apply(int32_t target, int32_t acceleration, uint32_t dt);
#endif
