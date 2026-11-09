#include "arm_debug_protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static bool valid_tokens(const char *line)
{
    char copy[80];
    char *tokens[5];
    unsigned count = 0;
    if (!line || strlen(line) >= sizeof(copy)) return false;
    strcpy(copy, line);
    for (char *token = strtok(copy, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
        if (count == 5) return false;
        tokens[count++] = token;
    }
    if (!count) return false;
    const char *verb = tokens[0];
    unsigned min = 0, max = 0, numeric = 2;
    if (!strcmp(verb,"HB") || !strcmp(verb,"PING") || !strcmp(verb,"STATUS") ||
        !strcmp(verb,"?") || !strcmp(verb,"STOP") || !strcmp(verb,"CLR") || !strcmp(verb,"CLEAR")) min = max = 1;
    else if (!strcmp(verb,"EN")) { min = max = 2; numeric = 1; }
    else if (!strcmp(verb,"HOME")) min = max = 2;
    else if (!strcmp(verb,"MOVE") || !strcmp(verb,"TRAVEL") || !strcmp(verb,"SETLIM") || !strcmp(verb,"LIMIT")) { min = 3; max = 4; }
    else if (!strcmp(verb,"JOG") || !strcmp(verb,"SETPOS") || !strcmp(verb,"SETVEL") || !strcmp(verb,"MOVEVEL")) min = max = 3;
    if (count < min || count > max) return false;
    for (unsigned i = numeric; i < count; ++i) {
        char *end;
        errno = 0;
        long value = strtol(tokens[i], &end, 10);
        if (errno || *end || end == tokens[i] || value < INT32_MIN || value > INT32_MAX) return false;
        if (numeric == 1 && value != 0 && value != 1) return false;
    }
    return true;
}

static bool parse_axis(const char *text, ArmDebugCommand *command)
{
    if (text == NULL || command == NULL) {
        return false;
    }

    if (strcmp(text, "Z") == 0 || strcmp(text, "z") == 0) {
        command->axis = ARM_VIRTUAL_AXIS_Z;
        command->physical_axis_selected = false;
        return true;
    }

    if (strcmp(text, "X") == 0 || strcmp(text, "x") == 0) {
        command->axis = ARM_VIRTUAL_AXIS_X;
        command->physical_axis = ARM_AXIS_X;
        command->physical_axis_selected = false;
        return true;
    }

    if (strcmp(text, "ZL") == 0 || strcmp(text, "zl") == 0) {
        command->physical_axis = ARM_AXIS_Z_LEFT;
        command->physical_axis_selected = true;
        return true;
    }

    if (strcmp(text, "ZR") == 0 || strcmp(text, "zr") == 0) {
        command->physical_axis = ARM_AXIS_Z_RIGHT;
        command->physical_axis_selected = true;
        return true;
    }

    return false;
}

bool arm_debug_parse_line(const char *line, ArmDebugCommand *command)
{
    char verb[12] = {0};
    char axis_text[4] = {0};
    int value = 0;
    int optional_value = 0;
    int scanned = 0;

    if (line == NULL || command == NULL || !valid_tokens(line)) {
        return false;
    }

    command->type = ARM_DEBUG_CMD_NONE;
    command->enabled = false;
    command->axis = ARM_VIRTUAL_AXIS_Z;
    command->physical_axis = ARM_AXIS_Z_LEFT;
    command->physical_axis_selected = false;
    command->velocity_um_s = 0;
    command->position_um = 0;
    command->velocity_limit_um_s = 0;
    command->min_position_um = 0;
    command->max_position_um = 0;

    if (sscanf(line, "%11s", verb) != 1) {
        return false;
    }

    if (strcmp(verb, "HB") == 0 || strcmp(verb, "PING") == 0) {
        command->type = ARM_DEBUG_CMD_HEARTBEAT;
        return true;
    }

    if (strcmp(verb, "STATUS") == 0 || strcmp(verb, "?") == 0) {
        command->type = ARM_DEBUG_CMD_STATUS;
        return true;
    }

    if (strcmp(verb, "STOP") == 0) {
        command->type = ARM_DEBUG_CMD_STOP;
        return true;
    }

    if (strcmp(verb, "CLR") == 0 || strcmp(verb, "CLEAR") == 0) {
        command->type = ARM_DEBUG_CMD_CLEAR_FAULT;
        return true;
    }

    if (strcmp(verb, "EN") == 0) {
        if (sscanf(line, "%*s %d", &value) != 1) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_ENABLE;
        command->enabled = value != 0;
        return true;
    }

    if (strcmp(verb, "JOG") == 0) {
        if (sscanf(line, "%*s %3s %d", axis_text, &value) != 2) {
            return false;
        }
        if (!parse_axis(axis_text, command)) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_JOG;
        command->velocity_um_s = (int32_t)value;
        return true;
    }

    if (strcmp(verb, "MOVE") == 0 || strcmp(verb, "TRAVEL") == 0) {
        scanned = sscanf(line, "%*s %3s %d %d", axis_text, &value, &optional_value);
        if (scanned < 2) {
            return false;
        }
        if (!parse_axis(axis_text, command)) {
            return false;
        }
        command->type = strcmp(verb, "TRAVEL") == 0 ? ARM_DEBUG_CMD_TRAVEL : ARM_DEBUG_CMD_MOVE;
        command->position_um = (int32_t)value;
        command->velocity_limit_um_s = scanned == 3 ? (int32_t)optional_value : 0;
        return true;
    }

    if (strcmp(verb, "SETPOS") == 0) {
        if (sscanf(line, "%*s %3s %d", axis_text, &value) != 2) {
            return false;
        }
        if (!parse_axis(axis_text, command)) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_SET_POSITION;
        command->position_um = (int32_t)value;
        return true;
    }

    if (strcmp(verb, "SETLIM") == 0 || strcmp(verb, "LIMIT") == 0) {
        scanned = sscanf(line, "%*s %3s %d %d", axis_text, &value, &optional_value);
        if (scanned < 2) {
            return false;
        }
        if (!parse_axis(axis_text, command)) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_SET_LIMIT;
        command->min_position_um = scanned == 3 ? (int32_t)value : 0;
        command->max_position_um = scanned == 3 ? (int32_t)optional_value : (int32_t)value;
        return true;
    }

    if (strcmp(verb, "SETVEL") == 0 || strcmp(verb, "MOVEVEL") == 0) {
        if (sscanf(line, "%*s %3s %d", axis_text, &value) != 2) {
            return false;
        }
        if (!parse_axis(axis_text, command)) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_SET_MOVE_VELOCITY;
        command->velocity_um_s = (int32_t)value;
        return true;
    }

    if (strcmp(verb, "HOME") == 0) {
        if (sscanf(line, "%*s %3s", axis_text) != 1) {
            return false;
        }
        if (!parse_axis(axis_text, command) || (command->physical_axis_selected &&
            (!ARM_STANDALONE_ZL || command->physical_axis != ARM_AXIS_Z_LEFT))) {
            return false;
        }
        command->type = ARM_DEBUG_CMD_HOME;
        return true;
    }

    return false;
}

static bool apply_command(ArmMotionState *state, const ArmDebugCommand *command)
{
    if (state == NULL || command == NULL) {
        return false;
    }

    switch (command->type) {
    case ARM_DEBUG_CMD_HEARTBEAT:
    case ARM_DEBUG_CMD_STATUS:
        return true;

    case ARM_DEBUG_CMD_ENABLE:
        if (command->enabled && state->faulted) return false;
        arm_motion_set_enabled(state, command->enabled);
        return true;

    case ARM_DEBUG_CMD_STOP:
        arm_motion_command_stop(state);
        return true;

    case ARM_DEBUG_CMD_CLEAR_FAULT:
        arm_motion_clear_fault(state);
        return true;

    case ARM_DEBUG_CMD_JOG:
        if (command->physical_axis_selected) {
            return arm_motion_command_axis_jog(
                state,
                command->physical_axis,
                command->velocity_um_s);
        }
        return arm_motion_command_jog(
            state,
            command->axis,
            command->velocity_um_s);

    case ARM_DEBUG_CMD_MOVE:
        if (command->physical_axis_selected) {
            return arm_motion_command_axis_move(
                state,
                command->physical_axis,
                command->position_um,
                command->velocity_limit_um_s);
        }
        return arm_motion_command_move(
            state,
            command->axis,
            command->position_um,
            command->velocity_limit_um_s);

    case ARM_DEBUG_CMD_SET_POSITION:
        if (command->physical_axis_selected) {
            return arm_motion_set_axis_position(
                state,
                command->physical_axis,
                command->position_um,
                true);
        }
        if (command->axis == ARM_VIRTUAL_AXIS_Z) {
            return arm_motion_set_axis_position(
                state,
                ARM_AXIS_Z_LEFT,
                command->position_um,
                true) &&
                arm_motion_set_axis_position(
                    state,
                    ARM_AXIS_Z_RIGHT,
                    command->position_um,
                    true);
        }
        return arm_motion_set_axis_position(
            state,
            ARM_AXIS_X,
            command->position_um,
            true);

    case ARM_DEBUG_CMD_SET_LIMIT:
        if (command->physical_axis_selected) {
            return arm_motion_set_axis_soft_limits(
                state,
                command->physical_axis,
                command->min_position_um,
                command->max_position_um);
        }
        if (command->axis == ARM_VIRTUAL_AXIS_Z) {
            return arm_motion_set_axis_soft_limits(
                state,
                ARM_AXIS_Z_LEFT,
                command->min_position_um,
                command->max_position_um) &&
                arm_motion_set_axis_soft_limits(
                    state,
                    ARM_AXIS_Z_RIGHT,
                    command->min_position_um,
                    command->max_position_um);
        }
        return arm_motion_set_axis_soft_limits(
            state,
            ARM_AXIS_X,
            command->min_position_um,
            command->max_position_um);

    case ARM_DEBUG_CMD_SET_MOVE_VELOCITY:
        if (command->physical_axis_selected) {
            return arm_motion_set_axis_default_move_velocity(
                state,
                command->physical_axis,
                command->velocity_um_s);
        }
        if (command->axis == ARM_VIRTUAL_AXIS_Z) {
            return arm_motion_set_axis_default_move_velocity(
                state,
                ARM_AXIS_Z_LEFT,
                command->velocity_um_s) &&
                arm_motion_set_axis_default_move_velocity(
                    state,
                    ARM_AXIS_Z_RIGHT,
                    command->velocity_um_s);
        }
        return arm_motion_set_axis_default_move_velocity(
            state,
            ARM_AXIS_X,
            command->velocity_um_s);

    case ARM_DEBUG_CMD_HOME:
        if (command->physical_axis_selected) return arm_motion_command_home_zl(state);
        return arm_motion_command_home(state, command->axis);

    case ARM_DEBUG_CMD_NONE:
    default:
        return false;
    }
}

bool arm_debug_apply_command(ArmMotionState *state, const ArmDebugCommand *command)
{
    if (!state || !command) return false;
    ArmMotionState next = *state;
    if (!apply_command(&next, command)) return false;
    *state = next;
    return true;
}

size_t arm_debug_format_status(
    const ArmMotionState *state,
    const ArmMotionOutputs *outputs,
    char *buffer,
    size_t buffer_size)
{
    int written = 0;

    if (state == NULL || outputs == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }

    written = snprintf(
        buffer,
        buffer_size,
        "STAT en=%u fault=%u code=%d axis=%d mode=%d "
        "zl=%ld zr=%ld x=%ld "
        "h_zl=%u h_zr=%u h_x=%u "
        "t_zl=%ld t_zr=%ld t_x=%ld "
        "lim_zl=%ld lim_zr=%ld lim_x=%ld "
        "mv_zl=%ld mv_zr=%ld mv_x=%ld "
        "f_zl=%lu f_zr=%lu f_x=%lu home=%u,%u,%u\r\n",
        state->enabled ? 1U : 0U,
        state->faulted ? 1U : 0U,
        (int)state->fault,
        (int)state->fault_axis,
        (int)state->mode,
        (long)state->axis[ARM_AXIS_Z_LEFT].position_um,
        (long)state->axis[ARM_AXIS_Z_RIGHT].position_um,
        (long)state->axis[ARM_AXIS_X].position_um,
        state->axis[ARM_AXIS_Z_LEFT].homed ? 1U : 0U,
        state->axis[ARM_AXIS_Z_RIGHT].homed ? 1U : 0U,
        state->axis[ARM_AXIS_X].homed ? 1U : 0U,
        state->axis[ARM_AXIS_Z_LEFT].target_position_active ?
            (long)state->axis[ARM_AXIS_Z_LEFT].target_position_um : -1L,
        state->axis[ARM_AXIS_Z_RIGHT].target_position_active ?
            (long)state->axis[ARM_AXIS_Z_RIGHT].target_position_um : -1L,
        state->axis[ARM_AXIS_X].target_position_active ?
            (long)state->axis[ARM_AXIS_X].target_position_um : -1L,
        (long)state->axis[ARM_AXIS_Z_LEFT].soft_max_position_um,
        (long)state->axis[ARM_AXIS_Z_RIGHT].soft_max_position_um,
        (long)state->axis[ARM_AXIS_X].soft_max_position_um,
        (long)state->axis[ARM_AXIS_Z_LEFT].default_move_velocity_um_s,
        (long)state->axis[ARM_AXIS_Z_RIGHT].default_move_velocity_um_s,
        (long)state->axis[ARM_AXIS_X].default_move_velocity_um_s,
        (unsigned long)outputs->axis[ARM_AXIS_Z_LEFT].step_frequency_millihz,
        (unsigned long)outputs->axis[ARM_AXIS_Z_RIGHT].step_frequency_millihz,
        (unsigned long)outputs->axis[ARM_AXIS_X].step_frequency_millihz,
        (unsigned)state->axis[0].home_phase, (unsigned)state->axis[1].home_phase,
        (unsigned)state->axis[2].home_phase);

    if (written < 0) {
        return 0;
    }

    if ((size_t)written >= buffer_size) {
        return buffer_size - 1U;
    }

    return (size_t)written;
}
