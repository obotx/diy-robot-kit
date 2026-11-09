#include "base_drive.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

static float clamp(float x, float low, float high)
{
    return fmaxf(low, fminf(high, x));
}
static bool positive(float value, float maximum)
{
    return isfinite(value) && value > 0 && value <= maximum;
}
void base_drive_init(BaseDrive *d, const BaseDriveConfig *config)
{
    memset(d, 0, sizeof(*d));
    d->config = *config;
    d->configured = config->commissioned && positive(config->diameter_m, 1) &&
        positive(config->wheelbase_m, 3) && positive(config->track_m, 3) &&
        positive(config->counts_per_turn, 1000000) && positive(config->wheel_max_mps, 10) &&
        positive(config->pwm_limit, 1) && positive(config->kp, 10) && positive(config->ki, 20);
    for (int i = 0; i < 4; ++i) {
        if (abs(config->motor_sign[i]) != 1 || abs(config->encoder_sign[i]) != 1)
            d->configured = false;
    }
}
bool base_drive_ready(const BaseDrive *d)
{
    return d && d->configured && !d->fault;
}
void base_drive_stop(BaseDrive *d)
{
    if (!d) return;
    d->enabled = false;
    d->service = false;
    memset(d->target, 0, sizeof(d->target));
    memset(d->velocity, 0, sizeof(d->velocity));
    memset(d->wheel_target, 0, sizeof(d->wheel_target));
    memset(d->pwm, 0, sizeof(d->pwm));
    memset(d->integral, 0, sizeof(d->integral));
    memset(d->no_feedback_ms, 0, sizeof(d->no_feedback_ms));
    memset(d->feedback_direction, 0, sizeof(d->feedback_direction));
    memset(d->feedback_seen, 0, sizeof(d->feedback_seen));
}
void base_drive_fault(BaseDrive *d, const char *fault)
{
    if (!d->fault) d->fault = fault;
    base_drive_stop(d);
}
/* Bench test: one wheel at a time. */
bool base_drive_service(BaseDrive *d, int wheel, float pwm, uint32_t now_ms)
{
    if (!d || d->fault || wheel < 0 || wheel > 3 || !isfinite(pwm) || fabsf(pwm) > .15f)
        return false;
    if (d->enabled && !d->service) return false;
    if (d->service && d->service_wheel != wheel) return false;
    if (!d->service) {
        base_drive_stop(d);
        d->service_started_ms = now_ms;
    }
    if ((uint32_t)(now_ms - d->service_started_ms) >= 3000U) {
        base_drive_fault(d, "wheel_test_timeout"); return false;
    }
    d->service = true; d->enabled = true; d->service_wheel = wheel;
    memset(d->pwm, 0, sizeof(d->pwm));
    d->pwm[wheel] = pwm; /* Electrical direction test, independent of calibration signs. */
    return true;
}
void base_drive_command(BaseDrive *d, bool enabled, const double velocity[3],
                        const double acceleration[2])
{
    if (!enabled || !base_drive_ready(d)) { base_drive_stop(d); return; }
    for (int i = 0; i < 3; ++i) d->target[i] = (float)velocity[i];
    for (int i = 0; i < 2; ++i) d->acceleration[i] = (float)acceleration[i];
    d->enabled = true;
}
void base_drive_tick(BaseDrive *d, const int32_t delta[4], uint32_t dt_ms)
{
    for (int i = 0; i < 4; ++i) {
        int64_t count = (int64_t)d->encoder_counts[i] + delta[i];
        d->encoder_counts[i] = count > INT32_MAX ? INT32_MAX : count < INT32_MIN ? INT32_MIN : (int32_t)count;
    }
    if (d->service && d->enabled) {
        if (!dt_ms || dt_ms > 30) base_drive_fault(d, "control_tick_overrun");
        return; /* Open-loop bench test deliberately does not require encoder calibration. */
    }
    if (!d->configured) { base_drive_stop(d); return; }
    if (!dt_ms || dt_ms > 30) { base_drive_fault(d, "control_tick_overrun"); return; }
    float dt = dt_ms * 0.001f;
    float meters_per_count = 3.14159265358979323846f * d->config.diameter_m /
        d->config.counts_per_turn;
    for (int i = 0; i < 4; ++i) {
        float measured = (float)delta[i] * d->config.encoder_sign[i] * meters_per_count / dt;
        if (fabsf(measured) > d->config.wheel_max_mps * 2.0f + 0.1f) {
            base_drive_fault(d, "encoder_speed_invalid"); return;
        }
        d->measured[i] += 0.25f * (measured - d->measured[i]);
    }
    if (!d->enabled) return;
    if (!base_drive_ready(d)) { base_drive_fault(d, "drive_not_ready"); return; }
    float dx = d->target[0] - d->velocity[0], dy = d->target[1] - d->velocity[1];
    float distance = hypotf(dx, dy), step = d->acceleration[0] * dt;
    float scale = distance > step ? step / distance : 1.0f;
    d->velocity[0] += dx * scale;
    d->velocity[1] += dy * scale;
    d->velocity[2] += clamp(d->target[2] - d->velocity[2],
                            -d->acceleration[1] * dt, d->acceleration[1] * dt);
    float x = d->velocity[0], y = d->velocity[1];
    float r = d->velocity[2] * (d->config.wheelbase_m + d->config.track_m) * 0.5f;
    /* X-pattern mecanum rollers viewed from above. +x forward, +y left, +yaw CCW. */
    float wheels[4] = {x - y - r, x + y + r, x + y - r, x - y + r};
    float peak = d->config.wheel_max_mps;
    for (int i = 0; i < 4; ++i) peak = fmaxf(peak, fabsf(wheels[i]));
    for (int i = 0; i < 4; ++i) {
        float target = wheels[i] * d->config.wheel_max_mps / peak;
        d->wheel_target[i] = target;
        float error = target - d->measured[i];
        float integral = clamp(d->integral[i] + d->config.ki * error * dt,
                               -d->config.pwm_limit, d->config.pwm_limit);
        /* Encoder PI control: no unmeasured full-duty speed feedforward. */
        float demand = d->config.kp * error + integral;
        /* Conditional integration prevents winding up against the duty cap. */
        if (fabsf(demand) <= d->config.pwm_limit || demand * error < 0)
            d->integral[i] = integral;
        demand = d->config.kp * error + d->integral[i];
        /* No active reverse torque near zero; PWM=0 brakes in sign-magnitude mode. */
        if (fabsf(target) < 0.0001f || target * demand < 0) {
            demand = 0;
            d->integral[i] = 0;
        }
        float pwm = clamp(demand, -d->config.pwm_limit, d->config.pwm_limit);
        int direction = fabsf(target) < 0.0001f ? 0 : (target > 0 ? 1 : -1);
        if (direction != d->feedback_direction[i]) {
            d->feedback_direction[i] = direction;
            d->feedback_seen[i] = false;
            d->no_feedback_ms[i] = 0;
        }
        if ((float)delta[i] * d->config.encoder_sign[i] * target > 0)
            d->feedback_seen[i] = true;
        if (fabsf(pwm) >= 0.1f && fabsf(target) >= 0.02f) {
            if ((float)delta[i] * d->config.encoder_sign[i] * target <= 0)
                d->no_feedback_ms[i] += dt_ms;
            else d->no_feedback_ms[i] = 0;
            /* Allow breakaway time, then use the shorter running-stall deadline. */
            if (d->no_feedback_ms[i] >= (d->feedback_seen[i] ? 500U : 1000U)) {
                base_drive_fault(d, "encoder_no_forward_feedback"); return;
            }
        } else d->no_feedback_ms[i] = 0;
        d->pwm[i] = pwm * d->config.motor_sign[i];
    }
}
