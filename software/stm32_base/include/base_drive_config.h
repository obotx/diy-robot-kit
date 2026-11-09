#ifndef BASE_DRIVE_CONFIG_H
#define BASE_DRIVE_CONFIG_H
#include "base_drive.h"

/* User measurements: 600 x 500 mm wheel centers, about 152.4 mm diameter.
 * 3450 is the observed quadrature count per WHEEL revolution; do not multiply x4.
 * Geometry is configured; PI gains still require physical validation.
 */
static const BaseDriveConfig BASE_DRIVE_CONFIG = {
    .commissioned = true,
    .diameter_m = 0.1524f,
    .wheelbase_m = 0.6f,
    .track_m = 0.5f,
    .counts_per_turn = 3450.0f,
    .wheel_max_mps = 0.6f, /* Wheel setpoint ceiling, not full-duty speed. */
    .pwm_limit = 0.90f,   /* Loaded-platform trial cap; not a current limit. */
    .kp = 0.4f,          /* Loaded-platform trial duty/(m/s); validate on hardware. */
    .ki = 1.0f,          /* Trial duty/m; anti-windup is implemented. */
    .motor_sign = {1, 1, 1, 1},
    /* All four signs verified against forward wheel rotation. */
    .encoder_sign = {1, -1, 1, -1}
};
#endif
