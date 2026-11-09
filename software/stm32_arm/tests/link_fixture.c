#include "arm_link.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void check_partial_frames(void)
{
    ArmLink link;
    ArmMotionState motion;
    ArmMotionInputs inputs = {0};
    char reply[ARM_LINK_TX_SIZE];
    const char hello[] = "{\"type\":\"hello\",\"protocol\":1,"
        "\"session\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"seq\":1}\n";
    arm_motion_init(&motion);
    arm_link_init(&link, "ARM-TEST", "BOOT-1", false);
    assert(arm_link_feed(&link, &motion, &inputs, 0, '{', reply, sizeof(reply)) == 0);
    arm_link_tick(&link, &motion, &inputs, 249);
    assert(link.rx_len == 1 && !link.dropping);
    arm_link_tick(&link, &motion, &inputs, 250);
    assert(link.rx_len == 0 && link.dropping && !link.session_valid);
    arm_link_feed(&link, &motion, &inputs, 250, '\n', reply, sizeof(reply));
    size_t received = 0;
    for (size_t i = 0; i < sizeof(hello) - 1; ++i) {
        received += arm_link_feed(&link, &motion, &inputs, 251, hello[i], reply, sizeof(reply));
    }
    assert(received > 0 && link.session_valid);
    received = 0;
    for (size_t i = 0; i < sizeof(hello) - 1; ++i) {
        received += arm_link_feed(&link, &motion, &inputs, 252, hello[i], reply, 1);
    }
    assert(received == 0 && !link.session_valid);
}

/* Host-only fixture. Positions and interlocks here are intentionally synthetic. */
int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    check_partial_frames();
    ArmLink link;
    ArmMotionState motion;
    ArmMotionInputs inputs = {0};
    ArmMotionOutputs outputs;
    uint32_t now = 0;
    char reply[ARM_LINK_TX_SIZE];
    char control[120];
    bool at_start = true, responded = false;
    bool level_band = false;
    long level_lower = 0, level_upper = 0;
    int64_t physical[3] = {64000000,64000000,20000LL * ARM_X_DRIVER_PULSES_PER_REV}, fraction[3] = {0};
    arm_motion_init(&motion);
    inputs.motor_power_enabled = inputs.e_stop_ok = true;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) arm_motion_set_axis_position(&motion, i, 500000, true);
    arm_link_init(&link, "ARM-TEST", "BOOT-1", argc > 1 && strcmp(argv[1], "motion") == 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int c; (c = getchar()) != EOF;) {
        if (at_start && c == '@') {
            if (!fgets(control, sizeof(control), stdin)) break;
            if (strncmp(control, "plant ", 6) == 0) {
                unsigned ms=(unsigned)strtoul(control+6,NULL,10);
                for (unsigned j=0;j<ms;j++) {
                    now++;
                    for (int a=0;a<ARM_AXIS_COUNT;a++) {
                        fraction[a]+=(int64_t)motion.axis[a].current_velocity_um_s*ARM_AXIS_CONFIG[a].driver_pulses_per_rev;
                        int64_t denominator=(int64_t)ARM_AXIS_CONFIG[a].um_per_motor_rev*1000;
                        int64_t steps=fraction[a]/denominator;
                        fraction[a]%=denominator;
                        arm_motion_on_steps(&motion,(ArmAxisId)a,steps);
                        physical[a]+=steps*ARM_AXIS_CONFIG[a].um_per_motor_rev;
                        /* Use continuous virtual travel for switch position; the
                         * motion state itself still receives whole step counts. */
                        inputs.axis[a].min_limit_active=physical[a]<=0;
                    }
                    if (level_band)
                        inputs.level_active = motion.axis[1].position_um >= level_lower &&
                                              motion.axis[1].position_um <= level_upper;
                    arm_link_tick(&link,&motion,&inputs,now);
                    if (now%10U==0) arm_motion_tick(&motion,&inputs,10,&outputs);
                }
            } else if (strncmp(control, "time ", 5) == 0) {
                now = (uint32_t)strtoull(control + 5, NULL, 10);
                arm_link_tick(&link, &motion, &inputs, now);
            } else if (strncmp(control, "advance ", 8) == 0) {
                unsigned ms = (unsigned)strtoul(control + 8, NULL, 10);
                for (unsigned i = 0; i < ms; ++i) {
                    ++now;
                    arm_link_tick(&link, &motion, &inputs, now);
                    if (now % 10U == 0) arm_motion_tick(&motion, &inputs, 10, &outputs);
                }
            } else if (sscanf(control, "levelband %ld %ld", &level_lower, &level_upper) == 2) {
                level_band = true;
                inputs.level_active = motion.axis[1].position_um >= level_lower &&
                                      motion.axis[1].position_um <= level_upper;
            } else if (strncmp(control, "level ", 6) == 0) {
                level_band = false;
                inputs.level_active = strtoul(control+6,NULL,10) != 0;
            } else if (strncmp(control, "unhome", 6) == 0) {
                for (int i = 0; i < ARM_AXIS_COUNT; ++i) motion.axis[i].homed = false;
            } else if (strncmp(control, "fault", 5) == 0) {
                motion.faulted = true;
                motion.fault = ARM_FAULT_DRIVER_ALARM;
            } else if (strncmp(control, "limit", 5) == 0) {
                inputs.axis[2].max_limit_active = true;
                arm_link_tick(&link, &motion, &inputs, now);
            } else if (strncmp(control, "isr ", 4) == 0) {
                now = (uint32_t)strtoull(control + 4, NULL, 10);
                arm_link_watchdog_isr(&link, now);
            }
            printf("{\"enabled\":%s,\"expired\":%s,\"seq\":%llu,"
                   "\"session_valid\":%s,\"target\":[%ld,%ld,%ld],"
                   "\"velocity\":[%ld,%ld,%ld],\"accel\":[%ld,%ld,%ld],\"position\":[%ld,%ld,%ld]}\n",
                   motion.enabled ? "true" : "false", link.watchdog_expired ? "true" : "false",
                   (unsigned long long)link.seq, link.session_valid ? "true" : "false",
                   (long)motion.axis[0].target_velocity_um_s, (long)motion.axis[1].target_velocity_um_s,
                   (long)motion.axis[2].target_velocity_um_s, (long)motion.axis[0].current_velocity_um_s,
                   (long)motion.axis[1].current_velocity_um_s, (long)motion.axis[2].current_velocity_um_s,
                   (long)motion.axis[0].acceleration_um_s2, (long)motion.axis[1].acceleration_um_s2,
                   (long)motion.axis[2].acceleration_um_s2,
                   (long)motion.axis[0].position_um, (long)motion.axis[1].position_um,
                   (long)motion.axis[2].position_um);
            continue;
        }
        size_t n = arm_link_feed(&link, &motion, &inputs, now, (uint8_t)c, reply, sizeof(reply));
        if (n) {
            fwrite(reply, 1, n, stdout);
            responded = true;
        }
        at_start = c == '\n';
        if (at_start) {
            if (!responded) puts("NONE");
            responded = false;
        }
    }
    return 0;
}
