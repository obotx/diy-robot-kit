#ifndef ARM_LINK_H
#define ARM_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "arm_motion.h"

#define ARM_LINK_MAX_FRAME 2048U
#define ARM_LINK_TX_SIZE 1024U
#define ARM_LINK_WATCHDOG_MS 250U

typedef struct {
    char device_id[81];
    char boot_id[81];
    char session[33];
    uint64_t seq;
    bool session_valid;
    bool motion_available;
    bool operation_active;
    int32_t gripper_target_milli;
    int32_t gripper_accel_milli;
    unsigned operation_axis;
    unsigned reference_stage;
    unsigned reference_pending;
    volatile bool watchdog_armed;
    volatile bool watchdog_expired;
    volatile uint32_t last_setpoint_ms;
    volatile bool service_active;
    volatile bool service_burst_active;
    volatile bool service_release_required;
    volatile uint32_t service_started_ms;
    volatile uint32_t service_burst_started_ms;
    uint32_t frame_start_ms;
    size_t rx_len;
    bool dropping;
    const char *error;
    char rx[ARM_LINK_MAX_FRAME];
} ArmLink;

void arm_link_init(ArmLink *link, const char *device_id, const char *boot_id,
                   bool motion_available);
void arm_link_abort(ArmLink *link, ArmMotionState *motion, const char *reason);
/* Called independently from the parser, including from the 1 ms interrupt. */
bool arm_link_watchdog_isr(ArmLink *link, uint32_t now_ms);
void arm_link_tick(ArmLink *link, ArmMotionState *motion,
                   const ArmMotionInputs *inputs, uint32_t now_ms);
/* Returns one newline-terminated reply, or zero for partial/invalid framing. */
size_t arm_link_feed(ArmLink *link, ArmMotionState *motion,
                     const ArmMotionInputs *inputs, uint32_t now_ms, uint8_t byte,
                     char *reply, size_t capacity);

#endif
