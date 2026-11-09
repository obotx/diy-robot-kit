#ifndef ARM_MOTION_H
#define ARM_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#include "arm_axis_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARM_FAULT_NONE = 0,
    ARM_FAULT_NOT_ENABLED,
    ARM_FAULT_DRIVER_ALARM,
    ARM_FAULT_HARD_LIMIT,
    ARM_FAULT_SOFT_LIMIT,
    ARM_FAULT_Z_SKEW,
    ARM_FAULT_MOTOR_POWER_DISABLED,
    ARM_FAULT_BAD_COMMAND,
    ARM_FAULT_HOME_TIMEOUT,
    ARM_FAULT_HOME_SWITCH,
    ARM_FAULT_STEP_GENERATOR,
    ARM_FAULT_COMM_TIMEOUT
} ArmFaultCode;

typedef enum {
    ARM_MODE_IDLE = 0,
    ARM_MODE_JOG,
    ARM_MODE_MOVE,
    ARM_MODE_HOMING
} ArmMotionMode;

typedef enum {
    ARM_HOME_NONE = 0, ARM_HOME_START, ARM_HOME_CLEAR, ARM_HOME_SEEK,
    ARM_HOME_BACKOFF, ARM_HOME_LATCH, ARM_HOME_RELEASE, ARM_HOME_DONE
} ArmHomePhase;

typedef struct {
    bool candidate;
    bool stable;
    uint32_t elapsed_ms;
} ArmLimitFilter;

typedef enum {
    ARM_LEVEL_START = 0, ARM_LEVEL_CLEAR, ARM_LEVEL_REVERSE,
    ARM_LEVEL_ENTER, ARM_LEVEL_EXIT, ARM_LEVEL_BRAKE, ARM_LEVEL_CENTER,
    ARM_LEVEL_LIFT_BRAKE, ARM_LEVEL_LIFT
} ArmLevelPhase;

typedef struct {
    bool min_limit_active;
    bool max_limit_active;
    bool driver_alarm_active;
} ArmAxisInputs;

typedef struct {
    ArmAxisInputs axis[ARM_AXIS_COUNT];
    bool motor_power_enabled;
    bool e_stop_ok;
    bool level_active;
} ArmMotionInputs;

typedef struct {
    bool enabled;
    bool dir_positive;
    uint32_t step_frequency_millihz;
} ArmAxisOutput;

typedef struct {
    ArmAxisOutput axis[ARM_AXIS_COUNT];
    bool faulted;
    ArmFaultCode fault;
    ArmAxisId fault_axis;
} ArmMotionOutputs;

typedef struct {
    int32_t position_um;
    int64_t position_numerator;
    int32_t soft_min_position_um;
    int32_t soft_max_position_um;
    int32_t default_move_velocity_um_s;
    int32_t target_position_um;
    int32_t move_velocity_limit_um_s;
    int32_t target_velocity_um_s;
    int32_t current_velocity_um_s;
    int32_t acceleration_um_s2;
    bool target_position_active;
    bool homed;
    bool pulse_active;
    ArmLimitFilter min_filter;
    ArmLimitFilter max_filter;
    ArmHomePhase home_phase;
    int32_t home_phase_start_um;
} ArmAxisState;

typedef struct {
    ArmAxisState axis[ARM_AXIS_COUNT];
    bool enabled;
    bool faulted;
    ArmFaultCode fault;
    ArmAxisId fault_axis;
    ArmMotionMode mode;
    ArmVirtualAxisId homing_axis;
    bool standalone_home;
    uint8_t homing_latched_mask;
    int32_t homing_start_um[ARM_AXIS_COUNT];
    uint32_t homing_elapsed_ms;
    volatile bool service_bypass;
    bool service_positions_unreferenced;
    bool leveling;
    ArmLimitFilter level_filter;
    int32_t level_start_um[2];
    uint32_t level_elapsed_ms;
    bool level_tip_down;
    ArmLevelPhase level_phase;
    int32_t level_first_um[2];
    bool paired_reference;
} ArmMotionState;

void arm_motion_init(ArmMotionState *state);
void arm_motion_set_enabled(ArmMotionState *state, bool enabled);
void arm_motion_clear_fault(ArmMotionState *state);

bool arm_motion_command_jog(
    ArmMotionState *state,
    ArmVirtualAxisId axis,
    int32_t velocity_um_s);

bool arm_motion_command_axis_jog(
    ArmMotionState *state,
    ArmAxisId axis,
    int32_t velocity_um_s);

bool arm_motion_command_move(
    ArmMotionState *state,
    ArmVirtualAxisId axis,
    int32_t target_position_um,
    int32_t velocity_limit_um_s);

bool arm_motion_command_axis_move(
    ArmMotionState *state,
    ArmAxisId axis,
    int32_t target_position_um,
    int32_t velocity_limit_um_s);

bool arm_motion_command_home(ArmMotionState *state, ArmVirtualAxisId axis);
bool arm_motion_command_home_zl(ArmMotionState *state);
bool arm_motion_command_level(ArmMotionState *state, bool tip_down);

void arm_motion_command_stop(ArmMotionState *state);

bool arm_motion_set_axis_position(
    ArmMotionState *state,
    ArmAxisId axis,
    int32_t position_um,
    bool mark_homed);

bool arm_motion_set_axis_soft_limits(
    ArmMotionState *state,
    ArmAxisId axis,
    int32_t min_position_um,
    int32_t max_position_um);

bool arm_motion_set_axis_default_move_velocity(
    ArmMotionState *state,
    ArmAxisId axis,
    int32_t velocity_um_s);

void arm_motion_on_step(
    ArmMotionState *state,
    ArmAxisId axis,
    bool dir_positive);

/* Completed STEP pulses, not motor encoder feedback. Called outside timer ISR. */
void arm_motion_on_steps(ArmMotionState *state, ArmAxisId axis, int64_t delta);
void arm_motion_latch_fault(ArmMotionState *state, ArmFaultCode fault, ArmAxisId axis);
int32_t arm_motion_jog_limit(const ArmAxisState *axis, int32_t requested, int32_t acceleration);
void arm_motion_skew_limit(const ArmMotionState *state, int32_t target[ARM_AXIS_COUNT],
                           const int32_t acceleration[ARM_AXIS_COUNT]);

void arm_motion_tick(
    ArmMotionState *state,
    const ArmMotionInputs *inputs,
    uint32_t dt_ms,
    ArmMotionOutputs *outputs);

#ifdef __cplusplus
}
#endif

#endif
