/* JSON v1 endpoint for the MDD10A base; parsing is independent of the timer ISR. */
#include "base_link.h"
#include "core_json.h"
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


void base_link_init(BaseLink *link, const char *device_id, const char *boot_id)
{
    memset(link, 0, sizeof(*link));
    snprintf(link->device_id, sizeof(link->device_id), "%s", device_id);
    snprintf(link->boot_id, sizeof(link->boot_id), "%s", boot_id);
}

void base_link_abort(BaseLink *link, const char *reason)
{
    base_drive_stop(link->drive);
    link->watchdog_armed = false;
    link->session_valid = false;
    link->error = reason;
}

bool base_link_watchdog_isr(BaseLink *link, uint32_t now_ms)
{
    if (link->watchdog_armed &&
        (uint32_t)(now_ms - link->last_setpoint_ms) >= BASE_LINK_WATCHDOG_MS) {
        link->watchdog_expired = true;
        link->session_valid = false;
        link->watchdog_armed = false;
    }
    return link->watchdog_expired;
}

void base_link_tick(BaseLink *link, uint32_t now_ms)
{
    if (link->drive && link->drive->service &&
        (uint32_t)(now_ms - link->drive->service_started_ms) >= 3000U)
        base_drive_fault(link->drive, "wheel_test_timeout");
    if (link->drive && link->drive->enabled && !link->drive->service && !base_drive_ready(link->drive))
        base_link_abort(link, "drive_not_ready");
    if (base_link_watchdog_isr(link, now_ms)) base_link_abort(link, "local_watchdog");
    if (link->rx_len && (uint32_t)(now_ms - link->frame_start_ms) >= BASE_LINK_WATCHDOG_MS) {
        base_link_abort(link, "frame_timeout");
        link->rx_len = 0;
        link->dropping = true;
    }
}

static size_t response(BaseLink *link, const char *session, uint64_t seq,
                       bool identity, bool accepted, char *buffer, size_t capacity)
{
    char envelope[320], seq_buffer[21];
    /* newlib-nano need not support 64-bit printf. */
    char *seq_text = seq_buffer + sizeof(seq_buffer) - 1;
    *seq_text = 0;
    do { *--seq_text = (char)('0' + seq % 10U); seq /= 10U; } while (seq);
    int n = snprintf(envelope, sizeof(envelope),
        "\"protocol\":1,\"session\":\"%s\",\"seq\":%s,"
        "\"role\":\"base\",\"device_id\":\"%s\",\"boot_id\":\"%s\"",
        session, seq_text, link->device_id, link->boot_id);
    if (n < 0 || (size_t)n >= sizeof(envelope)) goto overflow;
    if (identity) {
        n = snprintf(buffer, capacity,
            "{\"type\":\"identity\",%s,\"firmware\":\"base-mdd10a-5\","
            "\"capabilities\":[\"full_state_v1\",\"local_watchdog\",\"acceleration_limits\",\"base_wheel_test_v1\",\"base_clear_fault_v1\"],"
            "\"watchdog_ms\":250,\"enabled\":false,\"max_z_difference_mm\":null}\n", envelope);
    } else {
        char fault[180];
        const char *error = link->drive && link->drive->fault ? link->drive->fault : link->error;
        if (error) snprintf(fault, sizeof(fault), "\"%s\"", error);
        else strcpy(fault, "null");
        n = snprintf(buffer, capacity,
            "{\"type\":\"state\",%s,\"accepted\":%s,\"enabled\":%s,\"ready\":%s,"
            "\"fault\":%s,\"arm\":null,\"base\":{\"x_m\":null,\"y_m\":null,\"yaw_rad\":null,\"wheel_test_active\":%s,\"encoder_counts\":[%ld,%ld,%ld,%ld]}}\n",
            envelope, accepted ? "true" : "false",
            link->drive && link->drive->enabled ? "true" : "false",
            base_drive_ready(link->drive) ? "true" : "false", fault,
            link->drive && link->drive->service ? "true" : "false",
            link->drive ? (long)link->drive->encoder_counts[0] : 0L,
            link->drive ? (long)link->drive->encoder_counts[1] : 0L,
            link->drive ? (long)link->drive->encoder_counts[2] : 0L,
            link->drive ? (long)link->drive->encoder_counts[3] : 0L);
    }
    if (n < 0 || (size_t)n >= capacity) goto overflow;
    return (size_t)n;
overflow:
    base_link_abort(link, "reply_overflow");
    return 0;
}

static size_t handle(BaseLink *link, uint32_t now_ms, const char *data, size_t length,
                     char *reply, size_t capacity)
{
    Field fields[] = {{.name="type"}, {.name="protocol"}, {.name="session"}, {.name="seq"},
                      {.name="enabled"}, {.name="command"}, {.name="acceleration"}};
    char session[33];
    uint64_t seq, version;
    if (JSON_Validate(data, length) != JSONSuccess || !object_fields(data, length, fields, 7) ||
        !sequence(&fields[1], &version) || version != 1 ||
        !valid_session(&fields[2], session) || !sequence(&fields[3], &seq)) {
        base_link_abort(link, "invalid_envelope");
        return 0;
    }
    if (string_is(&fields[0], "hello") && !fields[4].found &&
        !fields[5].found && !fields[6].found) {
        base_link_abort(link, NULL);
        memcpy(link->session, session, sizeof(session));
        link->seq = seq;
        link->watchdog_expired = false;
        link->session_valid = true;
        return response(link, session, seq, true, true, reply, capacity);
    }
    const char *error = "invalid_setpoint";
    if (string_is(&fields[0], "clear_fault")) {
        if (fields[4].found || fields[5].found || fields[6].found) goto reject;
        if (link->watchdog_expired || !link->session_valid ||
            strcmp(session, link->session) != 0 || seq <= link->seq) {
            error = "invalid_session_or_sequence"; goto reject;
        }
        if (!link->drive || link->drive->enabled) {
            error = "release_before_clear"; goto reject;
        }
        base_drive_stop(link->drive);
        link->drive->fault = NULL;
        link->error = NULL;
        link->seq = seq;
        return response(link, session, seq, false, true, reply, capacity);
    }
    bool wheel_test = string_is(&fields[0], "wheel_setpoint");
    if ((!string_is(&fields[0], "setpoint") && !wheel_test) || !all_found(fields, 7) ||
        (fields[4].pair.jsonType != JSONTrue && fields[4].pair.jsonType != JSONFalse) ||
        fields[5].pair.jsonType != JSONObject || fields[6].pair.jsonType != JSONObject) goto reject;
    if (link->watchdog_expired) { error = "local_watchdog"; goto reject; }
    if (!link->session_valid || strcmp(session, link->session) != 0) {
        error = "invalid_session"; goto reject;
    }
    if (seq <= link->seq) { error = "invalid_sequence"; goto reject; }
    if (wheel_test) {
        Field test[] = {{.name="wheel"}, {.name="pwm"}};
        Field ramp[] = {{.name="base_mps2"}, {.name="rotation_radps2"}};
        double pwm, linear, rotation;
        if (!object_fields(fields[5].pair.value, fields[5].pair.valueLength, test, 2) ||
            !all_found(test, 2) || !number(&test[1], -.15, .15, &pwm) ||
            !object_fields(fields[6].pair.value, fields[6].pair.valueLength, ramp, 2) ||
            !all_found(ramp, 2) || !number(&ramp[0], .01, 1, &linear) ||
            !number(&ramp[1], .05, 2, &rotation)) goto reject;
        int wheel = string_is(&test[0], "FL") ? 0 : string_is(&test[0], "FR") ? 1 : string_is(&test[0], "RL") ? 2 :
                    string_is(&test[0], "RR") ? 3 : -1;
        bool on = fields[4].pair.jsonType == JSONTrue;
        if (!link->drive || wheel < 0 || (!on && pwm != 0)) {
            error = "wheel_test_invalid"; goto reject;
        }
        if (link->watchdog_expired || !link->session_valid) {
            error = "local_watchdog"; goto reject;
        }
        if (on && !base_drive_service(link->drive, wheel, (float)pwm, now_ms)) {
            error = "wheel_test_rejected"; goto reject;
        }
        if (!on) base_drive_stop(link->drive);
        link->last_setpoint_ms = now_ms;
        link->watchdog_armed = true;
        if (link->watchdog_expired || !link->session_valid) {
            error = "local_watchdog"; goto reject;
        }
        link->seq = seq; link->error = NULL;
        return response(link, session, seq, false, true, reply, capacity);
    }
    if (link->drive && link->drive->service && fields[4].pair.jsonType == JSONTrue) {
        error = "stop_before_mode_change"; goto reject;
    }
    Field cmd[] = {{.name="vx_mps"}, {.name="vy_mps"}, {.name="wz_radps"}};
    Field acc[] = {{.name="base_mps2"}, {.name="rotation_radps2"}};
    if (!object_fields(fields[5].pair.value, fields[5].pair.valueLength, cmd, 3) ||
        !object_fields(fields[6].pair.value, fields[6].pair.valueLength, acc, 2) ||
        !all_found(cmd, 3) || !all_found(acc, 2)) goto reject;
    double velocity[3], acceleration[2];
    if (!number(&cmd[0], -0.3, 0.3, &velocity[0]) ||
        !number(&cmd[1], -0.3, 0.3, &velocity[1]) ||
        !number(&cmd[2], -0.6, 0.6, &velocity[2]) ||
        !number(&acc[0], 0.01, 1.0, &acceleration[0]) ||
        !number(&acc[1], 0.05, 2.0, &acceleration[1]) ||
        hypot(velocity[0], velocity[1]) > 0.3) {
        error = "range_or_acceleration_limit"; goto reject;
    }
    bool enabled = fields[4].pair.jsonType == JSONTrue;
    if (enabled && !base_drive_ready(link->drive)) {
        error = !link->drive ? "motion_not_supported" :
            link->drive->fault ? link->drive->fault :
            "base_not_calibrated";
        goto reject;
    }
    if (!enabled && (velocity[0] != 0 || velocity[1] != 0 || velocity[2] != 0)) {
        error = "disabled_nonzero_command"; goto reject;
    }
    /* Only a validated, new full-state command refreshes the timer.
     * An ISR expiration while parsing is sticky and cannot be renewed here.
     * Never set session_valid here: only HELLO may establish a session. */
    link->last_setpoint_ms = now_ms;
    link->watchdog_armed = true;
    if (link->watchdog_expired || !link->session_valid) {
        error = "local_watchdog"; goto reject;
    }
    if (link->drive) base_drive_command(link->drive, enabled, velocity, acceleration);
    link->seq = seq;
    link->error = NULL;
    return response(link, session, seq, false, true, reply, capacity);
reject:
    base_link_abort(link, error);
    return response(link, session, seq, false, false, reply, capacity);
}

size_t base_link_feed(BaseLink *link, uint32_t now_ms, uint8_t byte,
                      char *reply, size_t capacity)
{
    base_link_tick(link, now_ms);
    if (link->dropping) {
        if (byte == '\n') link->dropping = false;
        return 0;
    }
    if (byte == '\n') {
        size_t length = link->rx_len;
        link->rx_len = 0;
        return handle(link, now_ms, link->rx, length, reply, capacity);
    }
    if (byte == 0 || byte > 127 || link->rx_len >= BASE_LINK_MAX_FRAME - 1U) {
        base_link_abort(link, "invalid_frame");
        link->rx_len = 0;
        link->dropping = true;
        return 0;
    }
    if (link->rx_len == 0) link->frame_start_ms = now_ms;
    link->rx[link->rx_len++] = (char)byte;
    return 0;
}
