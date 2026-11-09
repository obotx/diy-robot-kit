#ifndef ARM_DEBUG_PROTOCOL_H
#define ARM_DEBUG_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arm_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARM_DEBUG_CMD_NONE = 0,
    ARM_DEBUG_CMD_HEARTBEAT,
    ARM_DEBUG_CMD_ENABLE,
    ARM_DEBUG_CMD_STOP,
    ARM_DEBUG_CMD_CLEAR_FAULT,
    ARM_DEBUG_CMD_JOG,
    ARM_DEBUG_CMD_MOVE,
    ARM_DEBUG_CMD_SET_POSITION,
    ARM_DEBUG_CMD_SET_LIMIT,
    ARM_DEBUG_CMD_SET_MOVE_VELOCITY,
    ARM_DEBUG_CMD_HOME,
    ARM_DEBUG_CMD_STATUS,
    ARM_DEBUG_CMD_TRAVEL
} ArmDebugCommandType;

typedef struct {
    ArmDebugCommandType type;
    bool enabled;
    ArmVirtualAxisId axis;
    ArmAxisId physical_axis;
    bool physical_axis_selected;
    int32_t velocity_um_s;
    int32_t position_um;
    int32_t velocity_limit_um_s;
    int32_t min_position_um;
    int32_t max_position_um;
} ArmDebugCommand;

bool arm_debug_parse_line(const char *line, ArmDebugCommand *command);
bool arm_debug_apply_command(ArmMotionState *state, const ArmDebugCommand *command);
size_t arm_debug_format_status(
    const ArmMotionState *state,
    const ArmMotionOutputs *outputs,
    char *buffer,
    size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
