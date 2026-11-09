#include "arm_link.h"
#include "../vendor/coreJSON/core_json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    JSONPair_t pair;
    bool found;
} Field;

static bool text_is(const char *value, size_t length, const char *text)
{
    return strlen(text) == length && memcmp(value, text, length) == 0;
}

static bool string_is(const Field *field, const char *text)
{
    return field->found && field->pair.jsonType == JSONString &&
        text_is(field->pair.value, field->pair.valueLength, text);
}

/* Exact allowlists also reject duplicate and escaped field names. */
static bool object_fields(const char *data, size_t len, Field *fields, size_t count)
{
    size_t start = 0, next = 0;
    JSONPair_t pair;
    JSONStatus_t status;
    size_t first = 0;
    while (first < len && (data[first] == ' ' || data[first] == '\t' ||
           data[first] == '\r' || data[first] == '\n')) {
        ++first;
    }
    if (first == len || data[first] != '{') return false;
    while ((status = JSON_Iterate(data, len, &start, &next, &pair)) == JSONSuccess) {
        size_t i;
        for (i = 0; i < count; ++i) {
            if (pair.key && text_is(pair.key, pair.keyLength, fields[i].name)) break;
        }
        if (i == count || fields[i].found) return false;
        fields[i].pair = pair;
        fields[i].found = true;
    }
    return status == JSONNotFound;
}

static bool all_found(const Field *fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (!fields[i].found) return false;
    return true;
}

static bool sequence(const Field *field, uint64_t *result)
{
    if (!field->found || field->pair.jsonType != JSONNumber ||
        field->pair.valueLength == 0 || field->pair.valueLength > 16) return false;
    *result = 0;
    for (size_t i = 0; i < field->pair.valueLength; ++i) {
        char c = field->pair.value[i];
        if (c < '0' || c > '9') return false;
        *result = *result * 10U + (unsigned)(c - '0');
    }
    return *result <= UINT64_C(9007199254740991);
}

static bool number(const Field *field, double minimum, double maximum, double *out)
{
    char buffer[64], *end;
    if (!field->found || field->pair.jsonType != JSONNumber ||
        field->pair.valueLength >= sizeof(buffer)) return false;
    memcpy(buffer, field->pair.value, field->pair.valueLength);
    buffer[field->pair.valueLength] = 0;
    errno = 0;
    *out = strtod(buffer, &end);
    return errno != ERANGE && end == buffer + field->pair.valueLength &&
        isfinite(*out) && *out >= minimum && *out <= maximum;
}

static bool valid_session(const Field *field, char session[33])
{
    if (!field->found || field->pair.jsonType != JSONString ||
        field->pair.valueLength != 32) return false;
    for (size_t i = 0; i < 32; ++i) {
        char c = field->pair.value[i];
        if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'))) return false;
        session[i] = c;
    }
    session[32] = 0;
    return true;
}

void arm_link_init(ArmLink *link, const char *device_id, const char *boot_id,
                   bool motion_available)
{
    memset(link, 0, sizeof(*link));
    /* IDs are generated locally from hex digits, never supplied by the peer. */
    snprintf(link->device_id, sizeof(link->device_id), "%s", device_id);
    snprintf(link->boot_id, sizeof(link->boot_id), "%s", boot_id);
    link->motion_available = motion_available;
}

void arm_link_abort(ArmLink *link, ArmMotionState *motion, const char *reason)
{
    arm_motion_set_enabled(motion, false);
    link->session_valid = false;
    link->watchdog_armed = false;
    link->error = reason;
    link->operation_active = false;
    link->reference_stage = link->reference_pending = 0;
    link->gripper_target_milli = 0;
    link->service_active = false;
    link->service_burst_active = false;
    link->service_release_required = false;
}

bool arm_link_watchdog_isr(ArmLink *link, uint32_t now_ms)
{
    if (link->watchdog_armed &&
        (uint32_t)(now_ms - link->last_setpoint_ms) >= ARM_LINK_WATCHDOG_MS) {
        link->watchdog_expired = true;
    }
    return link->watchdog_expired;
}

static const char *hardware_fault(const ArmMotionState *motion, const ArmMotionInputs *inputs)
{
    if (motion->faulted) return "latched_motion_fault";
    if (!inputs->motor_power_enabled || !inputs->e_stop_ok) return "motor_power_or_estop";
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        if (inputs->axis[i].driver_alarm_active) return "driver_alarm";
    }
    return NULL;
}

static bool homed(const ArmMotionState *motion)
{
    for (int i = 0; i < (ARM_JSON_ZL_ONLY ? 1 : ARM_AXIS_COUNT); ++i) if (!motion->axis[i].homed) return false;
    return true;
}

/* Conservative envelope: command lease, braking distance and one control tick.
 * Applies to estimated positions only; commissioning still needs real feedback.
 */
static const char *motion_guard(const ArmMotionState *motion,
                               const ArmMotionInputs *inputs,
                               const int32_t target[ARM_AXIS_COUNT],
                               const int32_t acceleration[ARM_AXIS_COUNT])
{
    double negative[ARM_AXIS_COUNT], positive[ARM_AXIS_COUNT];
    for (int i = 0; i < (ARM_JSON_ZL_ONLY ? 1 : ARM_AXIS_COUNT); ++i) {
        const ArmAxisState *axis = &motion->axis[i];
        double current = axis->current_velocity_um_s;
        double low = fmin(0.0, fmin(current, target[i]));
        double high = fmax(0.0, fmax(current, target[i]));
        negative[i] = -low * 0.26 + low * low / (2.0 * acceleration[i]);
        positive[i] = high * 0.26 + high * high / (2.0 * acceleration[i]);
        if ((inputs->axis[i].min_limit_active && low < 0) ||
            (inputs->axis[i].max_limit_active && high > 0)) return "hard_limit";
        if (axis->position_um - negative[i] < axis->soft_min_position_um ||
            axis->position_um + positive[i] > axis->soft_max_position_um) return "soft_limit_margin";
    }
    return NULL;
}

void arm_link_tick(ArmLink *link, ArmMotionState *motion,
                   const ArmMotionInputs *inputs, uint32_t now_ms)
{
    if (arm_link_watchdog_isr(link, now_ms)) {
        arm_link_abort(link, motion, "local_watchdog");
    }
    if (link->rx_len && (uint32_t)(now_ms - link->frame_start_ms) >= ARM_LINK_WATCHDOG_MS) {
        arm_link_abort(link, motion, "frame_timeout");
        link->rx_len = 0;
        link->dropping = true;
    }
    if (motion->enabled) {
        const char *error = hardware_fault(motion, inputs);
        int32_t target[ARM_AXIS_COUNT], accel[ARM_AXIS_COUNT];
        for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
            target[i] = motion->axis[i].target_velocity_um_s;
            accel[i] = motion->axis[i].acceleration_um_s2;
        }
        if (!link->operation_active && !link->service_active)
            for (int i = 0; i < ARM_AXIS_COUNT; ++i)
                target[i] = arm_motion_jog_limit(&motion->axis[i], target[i], accel[i]);
        if (!error && !link->operation_active && !link->service_active) error = motion_guard(motion, inputs, target, accel);
        if (link->service_release_required) {
            for (int i=0; i<ARM_AXIS_COUNT; ++i)
                motion->axis[i].target_velocity_um_s = motion->axis[i].current_velocity_um_s = 0;
        }
        if (error) arm_link_abort(link, motion, error);
    }
    if (link->operation_active && motion->mode == ARM_MODE_IDLE) {
        if (!motion->faulted && link->reference_pending) link->reference_stage = link->reference_pending;
        link->reference_pending = 0;
        arm_motion_set_enabled(motion, false);
        link->operation_active = false;
    }
}

static void coordinate(const ArmAxisState *axis, char buffer[32])
{
    if (!axis->homed || axis->position_um < 0 || axis->position_um > 1000000) {
        strcpy(buffer, "null");
    } else {
        snprintf(buffer, 32, "%ld.%03ld", (long)(axis->position_um / 1000),
                 (long)(axis->position_um % 1000));
    }
}

static size_t response(ArmLink *link, ArmMotionState *motion, const ArmMotionInputs *inputs,
                       const char *session, uint64_t seq, bool identity, bool accepted,
                       char *buffer, size_t capacity)
{
    char envelope[320];
    /* newlib-nano does not require 64-bit printf support. */
    char seq_buffer[21];
    char *seq_text = seq_buffer + sizeof(seq_buffer) - 1;
    *seq_text = 0;
    do {
        *--seq_text = (char)('0' + seq % 10U);
        seq /= 10U;
    } while (seq);
    int n = snprintf(envelope, sizeof(envelope),
        "\"protocol\":1,\"session\":\"%s\",\"seq\":%s,"
        "\"role\":\"arm\",\"device_id\":\"%s\",\"boot_id\":\"%s\"",
        session, seq_text, link->device_id, link->boot_id);
    if (n < 0 || (size_t)n >= sizeof(envelope)) return 0;
    if (identity) {
        n = snprintf(buffer, capacity,
            "{\"type\":\"identity\",%s,\"firmware\":\"arm-link-2\","
            "\"capabilities\":[\"full_state_v1\",\"local_watchdog\","
            "\"acceleration_limits\",\"z_difference_limit\",\"arm_actions_v1\",\"gripper_velocity\"%s%s],"
            "\"watchdog_ms\":250,\"enabled\":false,\"max_z_difference_mm\":%ld.%03ld}\n",
            envelope, ARM_JSON_ZL_ONLY ? ",\"zl_only\"" : "",
            ARM_JSON_ZL_ONLY ? "" : ",\"service_jog_v1\",\"service_jog_v2\",\"x_microstep_128\",\"service_gripper_v1\",\"reference_sequence_v1\",\"reference_level_center_v1\",\"z_skew_soft_limit_v1\"",
            (long)(ARM_Z_MAX_SKEW_UM / 1000), (long)(ARM_Z_MAX_SKEW_UM % 1000));
    } else {
        char zl[32], zr[32], x[32], fault[180];
        const char *error = hardware_fault(motion, inputs);
        if (!error) error = link->error;
        if (error) snprintf(fault, sizeof(fault), "\"%s\"", error);
        else strcpy(fault, "null");
        coordinate(&motion->axis[0], zl);
        coordinate(&motion->axis[1], zr);
        coordinate(&motion->axis[2], x);
        bool ready = link->motion_available && homed(motion) && !error;
        n = snprintf(buffer, capacity,
            "{\"type\":\"state\",%s,\"accepted\":%s,\"enabled\":%s,\"ready\":%s,"
            "\"fault\":%s,\"arm\":{\"z_left_mm\":%s,\"z_right_mm\":%s,\"x_mm\":%s,"
            "\"homed\":%s,\"gripper_pct\":null,\"mode\":%u,\"active_axis\":%s,"
            "\"homed_axes\":{\"ZL\":%s,\"ZR\":%s,\"X\":%s},\"fault_code\":%u,"
            "\"service_active\":%s,\"service_release_required\":%s,"
            "\"reference_stage\":%u,\"level_active\":%s,"
            "\"limit_switches\":{\"ZL_MIN\":%s,\"ZL_MAX\":%s,\"ZR_MIN\":%s,\"ZR_MAX\":%s,\"X_MIN\":%s,\"X_MAX\":%s}},\"base\":null}\n",
            envelope, accepted ? "true" : "false", motion->enabled ? "true" : "false",
            ready ? "true" : "false", fault, zl, zr, x, homed(motion) ? "true" : "false",
            (unsigned)motion->mode, link->operation_active ?
            (link->operation_axis == 0 ? "\"ZL\"" : link->operation_axis == 1 ? "\"ZR\"" :
             link->operation_axis == 2 ? "\"X\"" : "\"Z\"") : "null",
            motion->axis[0].homed ? "true" : "false", motion->axis[1].homed ? "true" : "false",
            motion->axis[2].homed ? "true" : "false", (unsigned)motion->fault,
            link->service_active ? "true" : "false", link->service_release_required ? "true" : "false",
            link->reference_stage, inputs->level_active ? "true" : "false",
            inputs->axis[0].min_limit_active ? "true" : "false", inputs->axis[0].max_limit_active ? "true" : "false",
            inputs->axis[1].min_limit_active ? "true" : "false", inputs->axis[1].max_limit_active ? "true" : "false",
            inputs->axis[2].min_limit_active ? "true" : "false", inputs->axis[2].max_limit_active ? "true" : "false");
    }
    if (n < 0 || (size_t)n >= capacity) {
        arm_link_abort(link, motion, "reply_overflow");
        return 0;
    }
    return (size_t)n;
}

static size_t handle(ArmLink *link, ArmMotionState *motion, const ArmMotionInputs *inputs,
                     uint32_t now_ms, const char *data, size_t length,
                     char *reply, size_t capacity)
{
    Field fields[] = {{.name="type"}, {.name="protocol"}, {.name="session"}, {.name="seq"},
                      {.name="enabled"}, {.name="command"}, {.name="acceleration"},
                      {.name="action"}, {.name="axis"}, {.name="position_um"},
                      {.name="velocity_um_s"}, {.name="min_um"}, {.name="max_um"},
                      {.name="acceleration_um_s2"}};
    char session[33];
    uint64_t seq, version;
    if (JSON_Validate(data, length) != JSONSuccess ||
        !object_fields(data, length, fields, 14) ||
        !sequence(&fields[1], &version) || version != 1 ||
        !valid_session(&fields[2], session) || !sequence(&fields[3], &seq)) {
        arm_link_abort(link, motion, "invalid_envelope");
        return 0;
    }
    if (string_is(&fields[0], "hello") && !fields[4].found &&
        !fields[5].found && !fields[6].found && !fields[7].found && !fields[8].found &&
        !fields[9].found && !fields[10].found && !fields[11].found && !fields[12].found && !fields[13].found) {
        arm_link_abort(link, motion, NULL);
        memcpy(link->session, session, sizeof(session));
        link->seq = seq;
        link->watchdog_expired = false;
        link->session_valid = true;
        return response(link, motion, inputs, session, seq, true, true, reply, capacity);
    }
    const char *error = "invalid_setpoint";
    if (string_is(&fields[0], "arm_action")) {
        if (link->service_active) { error="exit_service_first"; goto reject; }
        error = "invalid_action";
        if (!link->session_valid || strcmp(session, link->session) || seq <= link->seq ||
            link->watchdog_expired || fields[4].found || fields[5].found || fields[6].found) goto reject;
        bool status = string_is(&fields[7], "status"), keep = string_is(&fields[7], "keepalive");
        bool stop = string_is(&fields[7], "stop"), clear = string_is(&fields[7], "clear_fault");
        bool home = string_is(&fields[7], "home"), move = string_is(&fields[7], "move");
        bool config = string_is(&fields[7], "configure");
        bool level_down = string_is(&fields[7], "level_down"), level_up = string_is(&fields[7], "level_up");
        bool level_center = string_is(&fields[7], "level_center");
        bool ref_z = string_is(&fields[7], "home_z"), lift = string_is(&fields[7], "lift_z");
        bool ref_x = string_is(&fields[7], "home_x");
        bool reference = level_down || level_up || level_center || ref_z || lift || ref_x;
        unsigned axis = string_is(&fields[8], "ZL") ? 0U : string_is(&fields[8], "ZR") ? 1U :
            string_is(&fields[8], "X") ? 2U : string_is(&fields[8], "Z") ? 3U : 4U;
        if (reference && (ARM_JSON_ZL_ONLY || fields[8].found)) goto reject;
        if (!(reference || status || keep || stop || clear || home || move || config) ||
            ((status || keep || stop || clear) && fields[8].found) ||
            ((home || move || config) && (axis == 4 || (ARM_JSON_ZL_ONLY && axis != 0))) ||
            ((!move && fields[9].found) || (!(move || config) && fields[10].found)) ||
            (!config && (fields[11].found || fields[12].found)) || (!move && fields[13].found)) goto reject;
        if (stop) { arm_motion_set_enabled(motion, false); link->operation_active = false; link->gripper_target_milli = 0; link->reference_pending = link->reference_stage = 0; }
        if (reference || clear || home || move || config) {
            if (motion->enabled || motion->mode != ARM_MODE_IDLE || link->operation_active) {
                error = "action_requires_disabled_idle"; goto reject;
            }
            if (clear) {
                if (!inputs->motor_power_enabled || !inputs->e_stop_ok) { error="motor_power_or_estop"; goto reject; }
                for (int i=0;i<ARM_AXIS_COUNT;i++) if (inputs->axis[i].driver_alarm_active ||
                    (inputs->axis[i].min_limit_active && inputs->axis[i].max_limit_active)) { error="active_input_fault"; goto reject; }
                arm_motion_clear_fault(motion);
                link->reference_pending = link->reference_stage = 0;
            } else {
                error = hardware_fault(motion, inputs);
                if (!error && !link->motion_available) error="hardware_not_commissioned";
                if (error) goto reject;
                ArmMotionState next = *motion;
                bool ok = false;
                double position=0, velocity=0, lower=0, upper=0;
                if (reference) {
                    unsigned required = ref_z ? 1U : lift ? 2U : ref_x ? 3U : 0U;
                    if (required && link->reference_stage != required) { error="reference_order"; goto reject; }
                    if (level_center && !inputs->level_active) { error="level_not_detected"; goto reject; }
                    arm_motion_set_enabled(&next, true);
                    if (level_down || level_up || level_center) {
                        ok = arm_motion_command_level(&next, level_down || level_center);
                        axis = 3;
                    } else if (lift) {
                        ok = arm_motion_command_move(&next, ARM_VIRTUAL_AXIS_Z, 200000, 20000);
                        axis = 3;
                    } else {
                        ok = arm_motion_command_home(&next, ref_x ? ARM_VIRTUAL_AXIS_X : ARM_VIRTUAL_AXIS_Z);
                        next.paired_reference = ref_z && ok;
                        axis = ref_x ? 2 : 3;
                    }
                    if (ok) {
                        if (!required) link->reference_stage = 0;
                        link->reference_pending = required + 1;
                    }
                } else if (home) {
                    if (fields[9].found || fields[10].found) goto reject;
                    for (int i=0;i<ARM_AXIS_COUNT;i++) if (inputs->axis[i].min_limit_active || inputs->axis[i].max_limit_active) {
                        error="home_switch_active"; goto reject;
                    }
                    arm_motion_set_enabled(&next, true);
                    ok = axis == 3 ? arm_motion_command_home(&next, ARM_VIRTUAL_AXIS_Z) :
                        axis == 2 ? arm_motion_command_home(&next, ARM_VIRTUAL_AXIS_X) :
                        axis == 0 && ARM_JSON_ZL_ONLY ? arm_motion_command_home_zl(&next) : false;
                } else if (move) {
                    double acceleration = 0;
                    if (fields[13].found &&
                        (!number(&fields[13], axis == 2 ? 10000 : 5000,
                                 axis == 2 ? ARM_X_MAX_ACCEL_UM_S2 : ARM_Z_MAX_ACCEL_UM_S2, &acceleration) ||
                         trunc(acceleration) != acceleration)) {
                        error="action_range"; goto reject;
                    }
                    if (!number(&fields[9],0,1000000,&position) || trunc(position)!=position ||
                        !number(&fields[10],1,axis==2 ? ARM_X_MAX_VELOCITY_UM_S : ARM_Z_MAX_VELOCITY_UM_S,&velocity) || trunc(velocity)!=velocity) {
                        error="action_range"; goto reject;
                    }
                    if (!ARM_JSON_ZL_ONLY && !homed(motion)) { error="not_homed"; goto reject; }
                    if (!ARM_JSON_ZL_ONLY && axis < 2 && fabs(position-motion->axis[1-axis].position_um)>ARM_Z_MAX_SKEW_UM) {
                        error="z_difference_limit"; goto reject;
                    }
                    arm_motion_set_enabled(&next,true);
                    if (fields[13].found) {
                        for (unsigned i=0; i<ARM_AXIS_COUNT; ++i)
                            if (axis==i || (axis==3 && i<2)) next.axis[i].acceleration_um_s2=(int32_t)acceleration;
                    }
                    ok = axis == 3 ? arm_motion_command_move(&next,ARM_VIRTUAL_AXIS_Z,(int32_t)position,(int32_t)velocity) :
                        arm_motion_command_axis_move(&next,(ArmAxisId)axis,(int32_t)position,(int32_t)velocity);
                } else {
                    if (!number(&fields[11],0,999999,&lower) || !number(&fields[12],1,1000000,&upper) ||
                        !number(&fields[10],1,axis==2 ? ARM_X_MAX_VELOCITY_UM_S : ARM_Z_MAX_VELOCITY_UM_S,&velocity) ||
                        trunc(lower)!=lower || trunc(upper)!=upper || trunc(velocity)!=velocity) { error="action_range"; goto reject; }
                    ok=true;
                    for (unsigned i=0;i<ARM_AXIS_COUNT;i++) if (axis==i || (axis==3 && i<2)) {
                        ok = ok && arm_motion_set_axis_soft_limits(&next,(ArmAxisId)i,(int32_t)lower,(int32_t)upper) &&
                            arm_motion_set_axis_default_move_velocity(&next,(ArmAxisId)i,(int32_t)velocity);
                    }
                }
                if (!ok) { error="action_precondition"; goto reject; }
                *motion=next;
                link->operation_active=reference || home || move;
                link->operation_axis=axis;
            }
        }
        link->seq=seq;
        link->error=NULL;
        if (!status) { link->last_setpoint_ms=now_ms; link->watchdog_armed=true; }
        return response(link,motion,inputs,session,seq,false,true,reply,capacity);
    }
    if (fields[7].found || fields[8].found || fields[9].found || fields[10].found || fields[11].found || fields[12].found || fields[13].found) goto reject;
    bool service = string_is(&fields[0], "service_setpoint");
    if ((!service && !string_is(&fields[0], "setpoint")) || !all_found(fields, 7) ||
        !link->session_valid || strcmp(session, link->session) != 0 || seq <= link->seq ||
        (fields[4].pair.jsonType != JSONTrue && fields[4].pair.jsonType != JSONFalse) ||
        fields[5].pair.jsonType != JSONObject || fields[6].pair.jsonType != JSONObject) goto reject;

    Field cmd[] = {{.name="zl_mps"}, {.name="zr_mps"}, {.name="x_mps"}, {.name="gripper"}};
    Field acc[] = {{.name="z_mps2"}, {.name="x_mps2"}, {.name="gripper_per_s"}};
    if (!object_fields(fields[5].pair.value, fields[5].pair.valueLength, cmd, 4) ||
        !object_fields(fields[6].pair.value, fields[6].pair.valueLength, acc, 3) ||
        !all_found(cmd, 4) || !all_found(acc, 3)) goto reject;
    double velocities[4], accelerations[3];
    if (!number(&cmd[0], -ARM_Z_MAX_VELOCITY_UM_S/1e6, ARM_Z_MAX_VELOCITY_UM_S/1e6, &velocities[0]) ||
        !number(&cmd[1], -ARM_Z_MAX_VELOCITY_UM_S/1e6, ARM_Z_MAX_VELOCITY_UM_S/1e6, &velocities[1]) ||
        !number(&cmd[2], -ARM_X_MAX_VELOCITY_UM_S/1e6, ARM_X_MAX_VELOCITY_UM_S/1e6, &velocities[2]) ||
        !number(&cmd[3], -1, 1, &velocities[3]) ||
        !number(&acc[0], 0.005, ARM_Z_MAX_ACCEL_UM_S2 / 1e6, &accelerations[0]) ||
        !number(&acc[1], 0.01, ARM_X_MAX_ACCEL_UM_S2 / 1e6, &accelerations[1]) ||
        !number(&acc[2], 0.02, 4, &accelerations[2])) {
        error = "range_or_acceleration_limit";
        goto reject;
    }
    bool enabled = fields[4].pair.jsonType == JSONTrue;
    if (service && ARM_JSON_ZL_ONLY) { error="service_requires_full_arm"; goto reject; }
    if (!service && enabled && link->service_active) { error="exit_service_first"; goto reject; }
    if (service && enabled && !link->service_active && motion->enabled) { error="release_before_service"; goto reject; }
    if (ARM_JSON_ZL_ONLY && (velocities[1] != 0 || velocities[2] != 0)) { error="inactive_axis"; goto reject; }
    if (enabled && link->operation_active) { error="operation_active"; goto reject; }
    for (int i = 0; i < 4; ++i) {
        if (!enabled && velocities[i] != 0) {
            error = "disabled_nonzero_command";
            goto reject;
        }
    }
    int32_t targets[ARM_AXIS_COUNT], accel[ARM_AXIS_COUNT];
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        targets[i] = (int32_t)llround(velocities[i] * 1e6);
        accel[i] = (int32_t)llround(accelerations[i == ARM_AXIS_X ? 1 : 0] * 1e6);
        if (abs(targets[i]) > ARM_AXIS_CONFIG[i].max_velocity_um_s) {
            error = "speed_limit";
            goto reject;
        }
    }
    if (enabled && service) {
        bool any = velocities[3] != 0;
        for (int i=0; i<ARM_AXIS_COUNT; ++i) {
            any |= targets[i] != 0;
        }
        if (!link->service_active && any) { error="service_requires_zero_entry"; goto reject; }
        if (!inputs->motor_power_enabled || !inputs->e_stop_ok) { error="motor_power_or_estop"; goto reject; }
        for (int i=0; i<ARM_AXIS_COUNT; ++i)
            if (inputs->axis[i].driver_alarm_active) { error="driver_alarm"; goto reject; }
        if (!link->motion_available) { error="hardware_not_commissioned"; goto reject; }
        /* Explicit zero-speed service entry may clear only limit-related faults. */
        if (!link->service_active && motion->faulted &&
            (motion->fault == ARM_FAULT_HARD_LIMIT || motion->fault == ARM_FAULT_SOFT_LIMIT ||
             motion->fault == ARM_FAULT_Z_SKEW || motion->fault == ARM_FAULT_HOME_SWITCH))
            arm_motion_clear_fault(motion);
        if (motion->faulted) { error="latched_motion_fault"; goto reject; }
        if (!link->service_active) {
            link->reference_stage = link->reference_pending = 0;
            link->service_started_ms = now_ms;
            link->service_active = true;
            motion->service_positions_unreferenced = true;
            link->service_burst_active = link->service_release_required = false;
            for (int i=0; i<ARM_AXIS_COUNT; ++i) motion->axis[i].homed = false;
        }
        if (!any) {
            link->service_burst_active = link->service_release_required = false;
        } else if (!link->service_burst_active) {
            link->service_burst_started_ms = now_ms;
            link->service_burst_active = true;
        }
        arm_link_watchdog_isr(link, now_ms);
        if (link->service_release_required) {
            for (int i=0; i<ARM_AXIS_COUNT; ++i) {
                targets[i] = 0;
                motion->axis[i].current_velocity_um_s = 0;
            }
        }
    } else if (enabled) {
        for (int i = 0; i < ARM_AXIS_COUNT; ++i)
            targets[i] = arm_motion_jog_limit(&motion->axis[i], targets[i], accel[i]);
        arm_motion_skew_limit(motion, targets, accel);
        error = hardware_fault(motion, inputs);
        if (!error && !link->motion_available) error = "hardware_not_commissioned";
        if (!error && !homed(motion)) error = "not_homed";
        if (!error) error = motion_guard(motion, inputs, targets, accel);
        if (error) goto reject;
    }
    /* Commit after validating the entire setpoint, preserving ramp velocity. */
    link->last_setpoint_ms = now_ms;
    link->watchdog_armed = true;
    if (link->watchdog_expired) {
        error = "local_watchdog";
        goto reject;
    }
    link->seq = seq;
    link->error = NULL;
    arm_motion_set_enabled(motion, enabled);
    link->gripper_target_milli = enabled ? (int32_t)llround(velocities[3] * 1000) : 0;
    link->gripper_accel_milli = (int32_t)llround(accelerations[2] * 1000);
    motion->service_bypass = enabled && service;
    if (!enabled) {
        if (link->operation_active) link->reference_stage = 0;
        link->reference_pending = 0;
        link->service_active = link->service_burst_active = link->service_release_required = false;
    }
    link->operation_active=false;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        motion->axis[i].target_position_active = false;
        motion->axis[i].move_velocity_limit_um_s = 0;
        motion->axis[i].target_velocity_um_s = targets[i];
        motion->axis[i].acceleration_um_s2 = accel[i];
    }
    motion->mode = enabled ? ARM_MODE_JOG : ARM_MODE_IDLE;
    return response(link, motion, inputs, session, seq, false, true, reply, capacity);
reject:
    arm_link_abort(link, motion, error);
    return response(link, motion, inputs, session, seq, false, false, reply, capacity);
}

size_t arm_link_feed(ArmLink *link, ArmMotionState *motion,
                     const ArmMotionInputs *inputs, uint32_t now_ms, uint8_t byte,
                     char *reply, size_t capacity)
{
    arm_link_tick(link, motion, inputs, now_ms);
    if (link->dropping) {
        if (byte == '\n') link->dropping = false;
        return 0;
    }
    if (byte == '\n') {
        size_t length = link->rx_len;
        link->rx_len = 0;
        return handle(link, motion, inputs, now_ms, link->rx, length, reply, capacity);
    }
    if (byte == 0 || byte > 127 || link->rx_len >= ARM_LINK_MAX_FRAME - 1U) {
        arm_link_abort(link, motion, "invalid_frame");
        link->rx_len = 0;
        link->dropping = true;
        return 0;
    }
    if (link->rx_len == 0) link->frame_start_ms = now_ms;
    link->rx[link->rx_len++] = (char)byte;
    return 0;
}
