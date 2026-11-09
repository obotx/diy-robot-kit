#include "arm_motion.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static bool valid_axis(ArmAxisId axis) { return (unsigned)axis < ARM_AXIS_COUNT; }
static int64_t magnitude(int64_t value) { return value < 0 ? -value : value; }

static bool moving(const ArmMotionState *s)
{
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        if (s->axis[i].pulse_active || s->axis[i].current_velocity_um_s ||
            s->axis[i].target_velocity_um_s) return true;
    }
    return false;
}

void arm_motion_command_stop(ArmMotionState *s)
{
    if (!s) return;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        s->axis[i].target_velocity_um_s = 0;
        s->axis[i].current_velocity_um_s = 0;
        s->axis[i].target_position_active = false;
        s->axis[i].move_velocity_limit_um_s = 0;
        s->axis[i].home_phase = ARM_HOME_NONE;
    }
    s->mode = ARM_MODE_IDLE;
    s->standalone_home = false;
    s->service_bypass = false;
    s->leveling = false;
    s->paired_reference = false;
}

void arm_motion_latch_fault(ArmMotionState *s, ArmFaultCode fault, ArmAxisId axis)
{
    if (!s) return;
    if (!s->faulted) {
        s->fault = fault;
        s->fault_axis = axis;
    }
    s->faulted = true;
    s->enabled = false;
    arm_motion_command_stop(s);
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) s->axis[i].homed = false;
}

void arm_motion_init(ArmMotionState *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        s->axis[i].soft_max_position_um = ARM_AXIS_CONFIG[i].travel_um;
        s->axis[i].default_move_velocity_um_s = ARM_AXIS_CONFIG[i].homing_fast_velocity_um_s;
        s->axis[i].acceleration_um_s2 = ARM_AXIS_CONFIG[i].max_accel_um_s2;
    }
}

void arm_motion_set_enabled(ArmMotionState *s, bool enabled)
{
    if (!s) return;
    if (!enabled) arm_motion_command_stop(s);
    s->enabled = enabled && !s->faulted;
}

void arm_motion_clear_fault(ArmMotionState *s)
{
    if (!s) return;
    arm_motion_set_enabled(s, false);
    s->faulted = false;
    s->fault = ARM_FAULT_NONE;
}

static bool can_command(const ArmMotionState *s)
{
    return s && s->enabled && !s->faulted && s->mode != ARM_MODE_HOMING;
}

bool arm_motion_command_axis_jog(ArmMotionState *s, ArmAxisId id, int32_t velocity)
{
    if (!can_command(s) || !valid_axis(id) ||
        magnitude(velocity) > ARM_AXIS_CONFIG[id].max_velocity_um_s) return false;
    if (ARM_STANDALONE_ZL && id != ARM_AXIS_Z_LEFT) return false;
    if (s->mode == ARM_MODE_MOVE) arm_motion_command_stop(s);
    s->mode = ARM_MODE_JOG;
    s->axis[id].target_position_active = false;
    s->axis[id].target_velocity_um_s = velocity;
    return true;
}

bool arm_motion_command_jog(ArmMotionState *s, ArmVirtualAxisId axis, int32_t velocity)
{
    if (ARM_STANDALONE_ZL) return false;
    if (axis == ARM_VIRTUAL_AXIS_X) return arm_motion_command_axis_jog(s, ARM_AXIS_X, velocity);
    if (axis != ARM_VIRTUAL_AXIS_Z || !can_command(s) ||
        magnitude(velocity) > ARM_Z_MAX_VELOCITY_UM_S) return false;
    return arm_motion_command_axis_jog(s, ARM_AXIS_Z_LEFT, velocity) &&
        arm_motion_command_axis_jog(s, ARM_AXIS_Z_RIGHT, velocity);
}

bool arm_motion_command_axis_move(ArmMotionState *s, ArmAxisId id, int32_t target, int32_t velocity)
{
    if (ARM_STANDALONE_ZL) return false;
    if (!can_command(s) || !valid_axis(id)) return false;
    ArmAxisState *a = &s->axis[id];
    if (!a->homed || target < a->soft_min_position_um || target > a->soft_max_position_um ||
        velocity < 0 || velocity > ARM_AXIS_CONFIG[id].max_velocity_um_s) return false;
    if (s->mode == ARM_MODE_JOG) arm_motion_command_stop(s);
    s->mode = ARM_MODE_MOVE;
    a->target_position_um = target;
    a->move_velocity_limit_um_s = velocity ? velocity : a->default_move_velocity_um_s;
    a->target_position_active = true;
    return true;
}

bool arm_motion_command_move(ArmMotionState *s, ArmVirtualAxisId axis, int32_t target, int32_t velocity)
{
    if (axis == ARM_VIRTUAL_AXIS_X) return arm_motion_command_axis_move(s, ARM_AXIS_X, target, velocity);
    if (axis != ARM_VIRTUAL_AXIS_Z || !s) return false;
    ArmMotionState next = *s;
    if (!arm_motion_command_axis_move(&next, ARM_AXIS_Z_LEFT, target, velocity) ||
        !arm_motion_command_axis_move(&next, ARM_AXIS_Z_RIGHT, target, velocity)) return false;
    *s = next;
    return true;
}

bool arm_motion_set_axis_position(ArmMotionState *s, ArmAxisId id, int32_t position, bool mark_homed)
{
    if (!s || !valid_axis(id) || moving(s) || s->mode == ARM_MODE_HOMING) return false;
    ArmAxisState *a = &s->axis[id];
    if (position < a->soft_min_position_um || position > a->soft_max_position_um) return false;
    a->position_um = position;
    a->position_numerator = (int64_t)position * ARM_AXIS_CONFIG[id].driver_pulses_per_rev;
    a->homed = mark_homed;
    a->target_position_active = false;
    return true;
}

bool arm_motion_set_axis_soft_limits(ArmMotionState *s, ArmAxisId id, int32_t min, int32_t max)
{
    if (!s || !valid_axis(id) || moving(s) || s->mode == ARM_MODE_HOMING || min < 0 ||
        min >= max || max > ARM_AXIS_CONFIG[id].travel_um) return false;
    ArmAxisState *a = &s->axis[id];
    if (a->homed && (a->position_um < min || a->position_um > max)) return false;
    a->soft_min_position_um = min;
    a->soft_max_position_um = max;
    return true;
}

bool arm_motion_set_axis_default_move_velocity(ArmMotionState *s, ArmAxisId id, int32_t velocity)
{
    if (!s || !valid_axis(id) || velocity <= 0 || velocity > ARM_AXIS_CONFIG[id].max_velocity_um_s)
        return false;
    s->axis[id].default_move_velocity_um_s = velocity;
    return true;
}

void arm_motion_on_steps(ArmMotionState *s, ArmAxisId id, int64_t delta)
{
    if (!s || !valid_axis(id)) return;
    /* A bounded batch also detects corrupted/unserviced hardware counters. */
    if (delta < -1000000 || delta > 1000000) {
        arm_motion_latch_fault(s, ARM_FAULT_STEP_GENERATOR, id);
        return;
    }
    ArmAxisState *a = &s->axis[id];
    int64_t numerator = a->position_numerator + delta * ARM_AXIS_CONFIG[id].um_per_motor_rev;
    int64_t position = numerator / ARM_AXIS_CONFIG[id].driver_pulses_per_rev;
    if (position < INT32_MIN || position > INT32_MAX) {
        arm_motion_latch_fault(s, ARM_FAULT_STEP_GENERATOR, id);
        return;
    }
    a->position_numerator = numerator;
    a->position_um = (int32_t)position;
}

void arm_motion_on_step(ArmMotionState *s, ArmAxisId id, bool positive)
{
    arm_motion_on_steps(s, id, positive ? 1 : -1);
}

static bool home_axis(const ArmMotionState *s, int id)
{
    return s->mode == ARM_MODE_HOMING &&
        (s->standalone_home ? id == ARM_AXIS_Z_LEFT :
        (s->homing_axis == ARM_VIRTUAL_AXIS_Z ? id != ARM_AXIS_X : id == ARM_AXIS_X));
}

static bool begin_home(ArmMotionState *s, ArmVirtualAxisId axis, bool standalone)
{
    if (!can_command(s) || moving(s) || (axis != ARM_VIRTUAL_AXIS_Z && axis != ARM_VIRTUAL_AXIS_X))
        return false;
    arm_motion_command_stop(s);
    s->mode = ARM_MODE_HOMING;
    s->standalone_home = standalone;
    s->homing_axis = axis;
    s->homing_elapsed_ms = 0;
    s->homing_latched_mask = 0;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        s->homing_start_um[i] = s->axis[i].position_um;
        if (home_axis(s, i)) {
            s->axis[i].homed = false;
            s->axis[i].home_phase = ARM_HOME_START;
            s->axis[i].home_phase_start_um = s->axis[i].position_um;
        }
    }
    return true;
}

bool arm_motion_command_home(ArmMotionState *s, ArmVirtualAxisId axis)
{
    return !ARM_STANDALONE_ZL && begin_home(s, axis, false);
}

bool arm_motion_command_home_zl(ArmMotionState *s)
{
    return (ARM_STANDALONE_ZL || ARM_JSON_ZL_ONLY) && begin_home(s, ARM_VIRTUAL_AXIS_Z, true);
}

bool arm_motion_command_level(ArmMotionState *s, bool tip_down)
{
    if (!can_command(s) || moving(s) || ARM_JSON_ZL_ONLY || ARM_STANDALONE_ZL) return false;
    arm_motion_command_stop(s);
    s->leveling = true;
    s->mode = ARM_MODE_JOG;
    s->service_positions_unreferenced = true;
    s->level_elapsed_ms = 0;
    s->level_tip_down = tip_down;
    s->level_phase = ARM_LEVEL_START;
    memset(&s->level_filter, 0, sizeof(s->level_filter));
    for (int i=0; i<ARM_AXIS_COUNT; ++i) s->axis[i].homed = false;
    for (int i=0; i<2; ++i) {
        s->level_start_um[i] = s->axis[i].position_um;
        s->axis[i].target_velocity_um_s = 0;
    }
    return true;
}

static void filter(ArmLimitFilter *f, bool raw, uint32_t dt)
{
    if (f->candidate != raw) {
        f->candidate = raw;
        f->elapsed_ms = 0;
    } else if (f->elapsed_ms < ARM_LIMIT_FILTER_MS) {
        f->elapsed_ms += dt > ARM_LIMIT_FILTER_MS ? ARM_LIMIT_FILTER_MS : dt;
        if (f->elapsed_ms >= ARM_LIMIT_FILTER_MS) f->stable = raw;
    }
}

static void home_phase(ArmAxisState *a, ArmHomePhase phase)
{
    a->home_phase = phase;
    a->home_phase_start_um = a->position_um;
    a->target_velocity_um_s = 0;
    a->current_velocity_um_s = 0;
}

static void home_tick(ArmMotionState *s, int id, const ArmAxisInputs *inputs)
{
    ArmAxisState *a = &s->axis[id];
    const ArmAxisConfig *cfg = &ARM_AXIS_CONFIG[id];
    int direction = cfg->home_direction;
    bool raw = direction < 0 ? inputs->min_limit_active : inputs->max_limit_active;
    bool stable = direction < 0 ? a->min_filter.stable : a->max_filter.stable;
    bool opposite = direction < 0 ? inputs->max_limit_active : inputs->min_limit_active;
    int64_t distance = magnitude((int64_t)a->position_um - a->home_phase_start_um);
    /* Accept a switch reached exactly at the seek boundary, but do not keep
     * searching past that boundary when the switch is missing. */
    bool seek_exhausted = distance >= ARM_ZL_HOME_SEEK_MAX_UM && !raw;
    if (s->standalone_home && (a->home_phase == ARM_HOME_SEEK ? seek_exhausted :
                              distance >= ARM_HOME_RELEASE_MAX_UM)) {
        arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, id);
        return;
    }
    if (opposite || (a->home_phase != ARM_HOME_SEEK && distance > ARM_HOME_RELEASE_MAX_UM) ||
        magnitude((int64_t)a->position_um - s->homing_start_um[id]) > cfg->travel_um + 5000LL) {
        arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, id);
        return;
    }
    /* Wait at Z phase boundaries: never back off while the other motor seeks. */
    if (!s->standalone_home && s->homing_axis == ARM_VIRTUAL_AXIS_Z &&
        a->home_phase > s->axis[id == ARM_AXIS_Z_LEFT ? ARM_AXIS_Z_RIGHT : ARM_AXIS_Z_LEFT].home_phase) {
        a->target_velocity_um_s = a->current_velocity_um_s = 0;
        return;
    }
    switch (a->home_phase) {
    case ARM_HOME_START:
        home_phase(a, raw ? ARM_HOME_CLEAR : ARM_HOME_SEEK);
        break;
    case ARM_HOME_CLEAR:
    case ARM_HOME_BACKOFF:
    case ARM_HOME_RELEASE:
        if (!raw && !stable && distance >= ARM_HOME_BACKOFF_UM) {
            a->target_velocity_um_s = a->current_velocity_um_s = 0;
            if (!a->pulse_active) {
                home_phase(a, a->home_phase == ARM_HOME_RELEASE ? ARM_HOME_DONE :
                    (a->home_phase == ARM_HOME_CLEAR ? ARM_HOME_SEEK : ARM_HOME_LATCH));
            }
        } else {
            a->target_velocity_um_s = -direction * (s->standalone_home ? 1000 : cfg->homing_slow_velocity_um_s);
        }
        break;
    case ARM_HOME_SEEK:
    case ARM_HOME_LATCH:
        if (raw) {
            a->target_velocity_um_s = a->current_velocity_um_s = 0;
            if (stable && !a->pulse_active) {
                home_phase(a, a->home_phase == ARM_HOME_SEEK ? ARM_HOME_BACKOFF : ARM_HOME_RELEASE);
            }
        } else {
            a->target_velocity_um_s = direction * (a->home_phase == ARM_HOME_SEEK ?
                (s->standalone_home ? 5000 : cfg->homing_fast_velocity_um_s) :
                (s->standalone_home ? 1000 : cfg->homing_slow_velocity_um_s));
        }
        break;
    case ARM_HOME_DONE:
        a->target_velocity_um_s = a->current_velocity_um_s = 0;
        break;
    default:
        arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, id);
        break;
    }
}

static int32_t ramp(int32_t current, int32_t target, int32_t acceleration, uint32_t dt)
{
    int64_t delta = (int64_t)acceleration * dt / 1000;
    if (current < target) return (int32_t)(current + delta > target ? target : current + delta);
    return (int32_t)(current - delta < target ? target : current - delta);
}

int32_t arm_motion_jog_limit(const ArmAxisState *a, int32_t requested, int32_t acceleration)
{
    if (!a->homed || !requested) return requested;
    if (acceleration <= 0) return 0;
    int64_t distance = requested < 0 ? (int64_t)a->position_um - a->soft_min_position_um :
                                     (int64_t)a->soft_max_position_um - a->position_um;
    /* Stay inside the independent 260 ms guard, allowing ramp/tick latency
     * and counted-pulse quantization. Solve d = v*t + v*v/(2*a). */
    double remaining = (double)distance - 20.0;
    if (remaining <= 0) return 0;
    double at = acceleration * 0.35;
    int32_t cap = (int32_t)(2.0 * acceleration * remaining /
                           (sqrt(at * at + 2.0 * acceleration * remaining) + at));
    if (magnitude(requested) <= cap) return requested;
    return requested < 0 ? -cap : cap;
}

void arm_motion_skew_limit(const ArmMotionState *s, int32_t target[ARM_AXIS_COUNT],
                           const int32_t acceleration[ARM_AXIS_COUNT])
{
    if (ARM_JSON_ZL_ONLY || s->service_bypass || s->leveling || s->mode == ARM_MODE_HOMING)
        return;
    if (!s->axis[0].homed || !s->axis[1].homed) return;
    int64_t skew = (int64_t)s->axis[0].position_um - s->axis[1].position_um;
    for (int i=0; i<2; ++i) {
        if (!target[i]) continue;
        bool increases = (i == 0) == (target[i] > 0);
        /* Reserve half of the available difference for each independently
         * ramped axis, including tick/pulse latency and quantization. */
        double distance = ((double)ARM_Z_MAX_SKEW_UM - (increases ? skew : -skew)) / 2.0 - 10.0;
        double a = acceleration[i];
        int32_t cap = 0;
        if (distance > 0 && a > 0) {
            double at = a * .35;
            cap = (int32_t)(2.0 * a * distance / (sqrt(at*at + 2.0*a*distance) + at));
        }
        if (magnitude(target[i]) > cap) target[i] = target[i] < 0 ? -cap : cap;
    }
}

static void move_tick(ArmAxisState *a, int id)
{
    if (!a->target_position_active) return;
    int64_t error = (int64_t)a->target_position_um - a->position_um;
    int32_t tolerance = ARM_AXIS_CONFIG[id].um_per_motor_rev /
        (2 * ARM_AXIS_CONFIG[id].driver_pulses_per_rev) + 1;
    if (magnitude(error) <= tolerance) {
        /* Never replace counted position with the requested target. */
        a->target_velocity_um_s = a->current_velocity_um_s = 0;
        if (!a->pulse_active) a->target_position_active = false;
        return;
    }
    /* The continuous sqrt(2*a*d) bound brakes too late for sampled control.
     * Reserve two 10 ms control intervals (including a pending STEP block).
     * Solve d = v*t + v*v/(2*a), retaining counted position at the endpoint. */
    double distance = (double)magnitude(error);
    double at = a->acceleration_um_s2 * 0.020;
    int32_t speed = (int32_t)(2.0 * a->acceleration_um_s2 * distance /
                             (sqrt(at * at + 2.0 * a->acceleration_um_s2 * distance) + at));
    if (speed > a->move_velocity_limit_um_s) speed = a->move_velocity_limit_um_s;
    a->target_velocity_um_s = error > 0 ? speed : -speed;
}

void arm_motion_tick(ArmMotionState *s, const ArmMotionInputs *inputs, uint32_t dt, ArmMotionOutputs *out)
{
    if (!s || !inputs || !out) return;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        filter(&s->axis[i].min_filter, inputs->axis[i].min_limit_active, dt);
        filter(&s->axis[i].max_filter, inputs->axis[i].max_limit_active, dt);
        if (inputs->axis[i].driver_alarm_active) arm_motion_latch_fault(s, ARM_FAULT_DRIVER_ALARM, i);
        if (!s->service_bypass && inputs->axis[i].min_limit_active && inputs->axis[i].max_limit_active)
            arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, i);
    }
    if (!inputs->motor_power_enabled || !inputs->e_stop_ok)
        arm_motion_latch_fault(s, ARM_FAULT_MOTOR_POWER_DISABLED, ARM_AXIS_Z_LEFT);
    int64_t skew = (int64_t)s->axis[0].position_um - s->axis[1].position_um;
    if (s->mode == ARM_MODE_HOMING && s->homing_axis == ARM_VIRTUAL_AXIS_Z)
        skew -= (int64_t)s->homing_start_um[0] - s->homing_start_um[1];
    bool isolated_home = ARM_STANDALONE_ZL && s->standalone_home;
    bool referenced_z = s->axis[0].homed && s->axis[1].homed;
    if (!s->service_bypass && !s->leveling && !ARM_JSON_ZL_ONLY && !isolated_home &&
        !(s->paired_reference && s->mode == ARM_MODE_HOMING && s->homing_axis == ARM_VIRTUAL_AXIS_Z) &&
        (!s->service_positions_unreferenced || referenced_z || s->mode != ARM_MODE_IDLE) &&
        magnitude(skew) > ARM_Z_MAX_SKEW_UM)
        arm_motion_latch_fault(s, ARM_FAULT_Z_SKEW, ARM_AXIS_Z_LEFT);
    if (s->leveling && s->enabled) {
        filter(&s->level_filter, inputs->level_active, dt);
        s->level_elapsed_ms += dt;
        if (s->level_elapsed_ms >= ARM_HOME_TIMEOUT_MS)
            arm_motion_latch_fault(s, ARM_FAULT_HOME_TIMEOUT, ARM_AXIS_Z_LEFT);
        for (int i=0; i<2 && s->leveling; ++i) {
            if (magnitude((int64_t)s->axis[i].position_um - s->level_start_um[i]) >= ARM_Z_TRAVEL_UM)
                arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, i);
        }
        if (s->leveling) {
            bool lower = inputs->axis[0].min_limit_active || inputs->axis[1].min_limit_active;
            if (lower && s->level_phase != ARM_LEVEL_LIFT_BRAKE && s->level_phase != ARM_LEVEL_LIFT) {
                s->level_phase = ARM_LEVEL_LIFT_BRAKE;
                for (int i=0; i<2; ++i) s->axis[i].target_position_active = false;
            }
            /* Hardware already inhibits downward pulses at the lower switch. */
            for (int i=0; i<2; ++i)
                if (inputs->axis[i].min_limit_active && s->axis[i].current_velocity_um_s < 0)
                    s->axis[i].current_velocity_um_s = 0;
            bool settled = s->level_filter.elapsed_ms >= ARM_LIMIT_FILTER_MS;
            if (s->level_phase == ARM_LEVEL_LIFT_BRAKE && !moving(s)) {
                for (int i=0; i<2; ++i) {
                    ArmAxisState *a = &s->axis[i];
                    a->target_position_um = a->position_um + 20000;
                    a->move_velocity_limit_um_s = 5000;
                    a->target_position_active = true;
                }
                s->level_phase = ARM_LEVEL_LIFT;
            } else if (s->level_phase == ARM_LEVEL_START && settled)
                s->level_phase = s->level_filter.stable ? ARM_LEVEL_CLEAR : ARM_LEVEL_ENTER;
            else if (s->level_phase == ARM_LEVEL_CLEAR && settled && !s->level_filter.stable)
                s->level_phase = ARM_LEVEL_REVERSE;
            else if (s->level_phase == ARM_LEVEL_REVERSE && !moving(s)) {
                s->level_tip_down = !s->level_tip_down;
                s->level_phase = ARM_LEVEL_ENTER;
            } else if (s->level_phase == ARM_LEVEL_ENTER && settled && s->level_filter.stable) {
                for (int i=0; i<2; ++i) s->level_first_um[i] = s->axis[i].position_um;
                s->level_phase = ARM_LEVEL_EXIT;
            } else if (s->level_phase == ARM_LEVEL_EXIT && settled && !s->level_filter.stable) {
                for (int i=0; i<2; ++i) {
                    ArmAxisState *a = &s->axis[i];
                    a->target_position_um = (int32_t)(((int64_t)s->level_first_um[i] + a->position_um) / 2);
                    a->move_velocity_limit_um_s = 5000;
                }
                s->level_phase = ARM_LEVEL_BRAKE;
            } else if (s->level_phase == ARM_LEVEL_BRAKE && !moving(s)) {
                for (int i=0; i<2; ++i) s->axis[i].target_position_active = true;
                s->level_phase = ARM_LEVEL_CENTER;
            }
            for (int i=0; i<2; ++i) {
                ArmAxisState *a = &s->axis[i];
                if (s->level_phase == ARM_LEVEL_CENTER || s->level_phase == ARM_LEVEL_LIFT) move_tick(a, i);
                else if (s->level_phase == ARM_LEVEL_CLEAR || s->level_phase == ARM_LEVEL_ENTER ||
                         s->level_phase == ARM_LEVEL_EXIT)
                    a->target_velocity_um_s = ((i == 1) == s->level_tip_down) ? 5000 : -5000;
                else a->target_velocity_um_s = 0;
            }
            if (s->level_phase == ARM_LEVEL_LIFT && !moving(s) &&
                !s->axis[0].target_position_active && !s->axis[1].target_position_active) {
                if (lower) arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, ARM_AXIS_Z_LEFT);
                else {
                    s->level_phase = ARM_LEVEL_START;
                    memset(&s->level_filter, 0, sizeof(s->level_filter));
                }
            }
            if (s->level_phase == ARM_LEVEL_CENTER && !moving(s) &&
                !s->axis[0].target_position_active && !s->axis[1].target_position_active && settled) {
                if (!s->level_filter.stable)
                    arm_motion_latch_fault(s, ARM_FAULT_HOME_SWITCH, ARM_AXIS_Z_LEFT);
                else {
                    s->leveling = false;
                    s->mode = ARM_MODE_IDLE;
                }
            }
        }
    }
    if (s->mode == ARM_MODE_HOMING) {
        uint32_t timeout = s->standalone_home ? ARM_ZL_HOME_TIMEOUT_MS : ARM_HOME_TIMEOUT_MS;
        if (dt >= timeout - s->homing_elapsed_ms) {
            arm_motion_latch_fault(s, ARM_FAULT_HOME_TIMEOUT, s->homing_axis == ARM_VIRTUAL_AXIS_X ? ARM_AXIS_X : ARM_AXIS_Z_LEFT);
        } else s->homing_elapsed_ms += dt;
    }
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        ArmAxisState *a = &s->axis[i];
        if (!s->enabled || s->faulted) continue;
        if (home_axis(s, i)) home_tick(s, i, &inputs->axis[i]);
        else if (!s->service_bypass) {
            if (s->mode == ARM_MODE_JOG)
                a->target_velocity_um_s = arm_motion_jog_limit(a, a->target_velocity_um_s, a->acceleration_um_s2);
            if (s->mode == ARM_MODE_MOVE) move_tick(a, i);
            bool negative = a->target_velocity_um_s < 0 || a->current_velocity_um_s < 0;
            bool positive = a->target_velocity_um_s > 0 || a->current_velocity_um_s > 0;
            if ((inputs->axis[i].min_limit_active && negative) || (inputs->axis[i].max_limit_active && positive))
                arm_motion_latch_fault(s, ARM_FAULT_HARD_LIMIT, i);
            if (a->homed && ((a->position_um <= a->soft_min_position_um && negative) ||
                (a->position_um >= a->soft_max_position_um && positive)))
                arm_motion_latch_fault(s, ARM_FAULT_SOFT_LIMIT, i);
        }
    }
    int32_t limited[ARM_AXIS_COUNT], acceleration[ARM_AXIS_COUNT];
    for (int i=0; i<ARM_AXIS_COUNT; ++i) {
        limited[i] = s->axis[i].target_velocity_um_s;
        acceleration[i] = s->axis[i].acceleration_um_s2;
    }
    arm_motion_skew_limit(s, limited, acceleration);
    for (int i=0; i<ARM_AXIS_COUNT; ++i) {
        s->axis[i].target_velocity_um_s = limited[i];
        if (s->enabled && !s->faulted)
            s->axis[i].current_velocity_um_s = ramp(s->axis[i].current_velocity_um_s,
                limited[i], acceleration[i], dt);
    }
    if (s->mode == ARM_MODE_HOMING) {
        bool done = true;
        for (int i = 0; i < ARM_AXIS_COUNT; ++i)
            if (home_axis(s, i) && (s->axis[i].home_phase != ARM_HOME_DONE || s->axis[i].pulse_active)) done = false;
        if (done) {
            for (int i = 0; i < ARM_AXIS_COUNT; ++i) if (home_axis(s, i)) {
                ArmAxisState *a = &s->axis[i];
                a->position_um = ARM_AXIS_CONFIG[i].home_direction < 0 ? a->soft_min_position_um : a->soft_max_position_um;
                a->position_numerator = (int64_t)a->position_um * ARM_AXIS_CONFIG[i].driver_pulses_per_rev;
                a->homed = true;
                s->homing_latched_mask |= (uint8_t)(1U << i);
            }
            s->mode = ARM_MODE_IDLE;
        }
    }
    if (s->mode == ARM_MODE_MOVE && !moving(s)) {
        bool target = false;
        for (int i = 0; i < ARM_AXIS_COUNT; ++i) target |= s->axis[i].target_position_active;
        if (!target) s->mode = ARM_MODE_IDLE;
    }
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        if (!s->enabled || s->faulted) s->axis[i].current_velocity_um_s = 0;
        out->axis[i].enabled = s->enabled && !s->faulted;
        out->axis[i].dir_positive = s->axis[i].current_velocity_um_s >= 0;
        out->axis[i].step_frequency_millihz = (uint32_t)(magnitude(s->axis[i].current_velocity_um_s) *
            ARM_AXIS_CONFIG[i].driver_pulses_per_rev * 1000 / ARM_AXIS_CONFIG[i].um_per_motor_rev);
    }
    out->faulted = s->faulted;
    out->fault = s->fault;
    out->fault_axis = s->fault_axis;
}
