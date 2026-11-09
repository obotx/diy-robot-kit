#ifndef ARM_AXIS_CONFIG_H
#define ARM_AXIS_CONFIG_H

#include <stdint.h>
#ifndef ARM_JSON_ZL_ONLY
#define ARM_JSON_ZL_ONLY 0
#endif
#ifndef ARM_STANDALONE_ZL
#define ARM_STANDALONE_ZL 0
#endif
#if ARM_STANDALONE_ZL && (!ARM_SERIAL_SERVICE_MODE || !ARM_BENCH_OUTPUTS)
#error "Standalone ZL requires an explicit service bench build"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARM_AXIS_Z_LEFT = 0,
    ARM_AXIS_Z_RIGHT = 1,
    ARM_AXIS_X = 2,
    ARM_AXIS_COUNT = 3
} ArmAxisId;

typedef enum {
    ARM_VIRTUAL_AXIS_Z = 0,
    ARM_VIRTUAL_AXIS_X = 1
} ArmVirtualAxisId;

typedef struct {
    const char *name;
    int32_t travel_um;
    int32_t um_per_motor_rev;
    int32_t driver_pulses_per_rev;
    int32_t max_velocity_um_s;
    int32_t max_accel_um_s2;
    int32_t homing_fast_velocity_um_s;
    int32_t homing_slow_velocity_um_s;
    int8_t home_direction;
} ArmAxisConfig;

#define ARM_Z_TRAVEL_UM              850000
#define ARM_X_TRAVEL_UM              950000

/* 100 mm is only for the isolated horizontal ZL bench, never the assembled arm. */
#if ARM_STANDALONE_ZL || ARM_JSON_ZL_ONLY
#define ARM_Z_MAX_SKEW_UM            100000
#else
#define ARM_Z_MAX_SKEW_UM            220000
#endif

/* KGX80-1000 configured for 5 mm per motor revolution. */
#define ARM_Z_UM_PER_REV             5000

/* DS60-1000 with HPS60-05SW 5:1 gearbox. Revised empirical travel at P001=16:
 * indicated 950 mm = measured 899 mm.
 * Previous effective lead 26467 * (899 / 950), rounded to micrometers.
 * This is a calibrated pulse scale, not a verified mechanical pulley lead. */
#define ARM_X_UM_PER_REV             25046

/* ZDM-2HA865 P001 = 16 microstep subdivision, 200 full steps/rev motor. */
#define ARM_DRIVER_PULSES_PER_REV    3200
#define ARM_X_DRIVER_PULSES_PER_REV  3200

/* Bench verified: reverse ZL electrical DIR so positive travel raises the carriage. */
#define ARM_ZL_DIR_INVERTED          1
#define ARM_ZR_DIR_INVERTED          1
#define ARM_X_DIR_INVERTED           0

/* Initial oscilloscope/bench envelope, not a commissioned machine rating. */
#define ARM_STEP_MAX_HZ              160000U
#define ARM_STEP_OPM_MAX_HZ          5000U
#define ARM_Z_STEP_MAX_VELOCITY_UM_S ((int32_t)(ARM_STEP_MAX_HZ * ARM_Z_UM_PER_REV / ARM_DRIVER_PULSES_PER_REV))
#define ARM_STEP_HIGH_US             8U
/* DL86H supplied manual: ENA -> DIR >5 ms, DIR -> STEP >100 us. */
#define ARM_DRIVER_ENABLE_SETUP_MS  10U
#define ARM_STEP_SETUP_US            150U
#define ARM_LIMIT_FILTER_MS          5U
#define ARM_HOME_BACKOFF_UM          1000
#define ARM_HOME_RELEASE_MAX_UM      10000
#define ARM_HOME_TIMEOUT_MS          600000U
/* Isolated ZL: full 1 m seek plus four bounded switch-handling phases. */
#define ARM_ZL_HOME_SEEK_MAX_UM       ARM_Z_TRAVEL_UM
#define ARM_ZL_HOME_TOTAL_MAX_UM      (ARM_ZL_HOME_SEEK_MAX_UM + 4 * ARM_HOME_RELEASE_MAX_UM)
#define ARM_ZL_HOME_TIMEOUT_MS        300000U

/* Motion limits; increase only after unloaded and loaded tests. */
#define ARM_Z_MAX_VELOCITY_UM_S      190000
#define ARM_Z_MAX_ACCEL_UM_S2        100000
#define ARM_X_MAX_VELOCITY_UM_S      500000
#define ARM_X_MAX_ACCEL_UM_S2        500000

static const ArmAxisConfig ARM_AXIS_CONFIG[ARM_AXIS_COUNT] = {
    {
        "z_left",
        ARM_Z_TRAVEL_UM,
        ARM_Z_UM_PER_REV,
        ARM_DRIVER_PULSES_PER_REV,
        ARM_Z_MAX_VELOCITY_UM_S,
        ARM_Z_MAX_ACCEL_UM_S2,
        5000,
        500,
        -1
    },
    {
        "z_right",
        ARM_Z_TRAVEL_UM,
        ARM_Z_UM_PER_REV,
        ARM_DRIVER_PULSES_PER_REV,
        ARM_Z_MAX_VELOCITY_UM_S,
        ARM_Z_MAX_ACCEL_UM_S2,
        5000,
        500,
        -1
    },
    {
        "x",
        ARM_X_TRAVEL_UM,
        ARM_X_UM_PER_REV,
        ARM_X_DRIVER_PULSES_PER_REV,
        ARM_X_MAX_VELOCITY_UM_S,
        ARM_X_MAX_ACCEL_UM_S2,
        25000,
        2000,
        -1
    }
};

#ifdef __cplusplus
}
#endif

#endif
