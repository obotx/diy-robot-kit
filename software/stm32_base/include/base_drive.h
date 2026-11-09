#ifndef BASE_DRIVE_H
#define BASE_DRIVE_H
#include <stdbool.h>
#include <stdint.h>

/* Wheel order: front-left, front-right, rear-left, rear-right. */
typedef struct {
    bool commissioned;
    float diameter_m, wheelbase_m, track_m;
    float counts_per_turn, wheel_max_mps, pwm_limit, kp, ki;
    int motor_sign[4], encoder_sign[4];
} BaseDriveConfig;

typedef struct {
    BaseDriveConfig config;
    bool configured;
    volatile bool enabled;
    bool service;
    int service_wheel;
    uint32_t service_started_ms;
    int32_t encoder_counts[4];
    const char *fault;
    float target[3], velocity[3], acceleration[2];
    float wheel_target[4], measured[4], integral[4], pwm[4];
    uint32_t no_feedback_ms[4];
    int feedback_direction[4];
    bool feedback_seen[4];
} BaseDrive;

void base_drive_init(BaseDrive *drive, const BaseDriveConfig *config);
bool base_drive_ready(const BaseDrive *drive);
bool base_drive_service(BaseDrive *drive, int wheel, float pwm, uint32_t now_ms);
void base_drive_stop(BaseDrive *drive);
void base_drive_fault(BaseDrive *drive, const char *fault);
/* Validated full-state command; repeated setpoints preserve ramp state. */
void base_drive_command(BaseDrive *drive, bool enabled, const double velocity[3],
                        const double acceleration[2]);
/* Hardware quadrature deltas, sampled every 10 ms, including while disabled. */
void base_drive_tick(BaseDrive *drive, const int32_t delta[4], uint32_t dt_ms);
#endif
