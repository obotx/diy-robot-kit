#ifdef NDEBUG
#undef NDEBUG
#endif
#include "arm_motion.h"
#include "arm_debug_protocol.h"
#include "arm_step_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArmMotionState s;
static ArmMotionInputs in;
static ArmMotionOutputs out;
static void reset(void) {
    arm_motion_init(&s);
    memset(&in, 0, sizeof(in)); memset(&out, 0, sizeof(out));
    in.motor_power_enabled = in.e_stop_ok = true;
    arm_motion_set_enabled(&s, true);
}
static void tick(void) { arm_motion_tick(&s, &in, 10, &out); }

static void counting(void) {
    reset(); assert(arm_motion_command_jog(&s, ARM_VIRTUAL_AXIS_X, 10000));
    for (int i=0;i<100;i++) tick();
    assert(s.axis[2].position_um == 0);
    arm_motion_command_stop(&s);
    for (int a=0;a<3;a++) {
        for (int i=0;i<ARM_AXIS_CONFIG[a].driver_pulses_per_rev;i++) arm_motion_on_step(&s,a,true);
        assert(s.axis[a].position_um == ARM_AXIS_CONFIG[a].um_per_motor_rev);
        for (int i=0;i<ARM_AXIS_CONFIG[a].driver_pulses_per_rev;i++) arm_motion_on_step(&s,a,false);
        assert(s.axis[a].position_um == 0 && s.axis[a].position_numerator == 0);
    }
    puts("PASS exact pulse counts, reversals, no elapsed-time position");
}

static void home_sim(bool z, bool initially_active, bool offset) {
    reset();
    int64_t physical[3] = {initially_active ? -100 : 1000, offset ? 1500 : 1000, initially_active ? -100 : 1000};
    int64_t fraction[3] = {0}; unsigned visited[3] = {0};
    for (int a=0;a<3;a++) {
        physical[a] *= ARM_AXIS_CONFIG[a].driver_pulses_per_rev;
        in.axis[a].min_limit_active = physical[a] <= 0;
    }
    assert(arm_motion_command_home(&s,z ? ARM_VIRTUAL_AXIS_Z : ARM_VIRTUAL_AXIS_X));
    for (unsigned ms=0;ms<60000 && s.mode == ARM_MODE_HOMING;ms+=10) {
        for (int a=0;a<3;a++) {
            if ((z && a==2) || (!z && a!=2)) continue;
            /* Independent simulated plant feeds counted pulses back into the controller. */
            fraction[a] += (int64_t)s.axis[a].current_velocity_um_s * ARM_AXIS_CONFIG[a].driver_pulses_per_rev * 10;
            int64_t divisor = (int64_t)ARM_AXIS_CONFIG[a].um_per_motor_rev * 1000;
            int64_t pulses = fraction[a] / divisor;
            fraction[a] %= divisor;
            arm_motion_on_steps(&s,a,pulses);
            physical[a] += pulses * ARM_AXIS_CONFIG[a].um_per_motor_rev;
            in.axis[a].min_limit_active = physical[a] <= 0;
            visited[a] |= 1U << s.axis[a].home_phase;
        }
        tick();
    }
    assert(!s.faulted && s.mode == ARM_MODE_IDLE);
    for (int a=0;a<3;a++) if ((z && a!=2) || (!z && a==2)) {
        assert(s.axis[a].homed && s.axis[a].position_um == 0);
        assert(visited[a] & (1U << ARM_HOME_SEEK));
        assert(visited[a] & (1U << ARM_HOME_BACKOFF));
        assert(visited[a] & (1U << ARM_HOME_LATCH));
        assert(visited[a] & (1U << ARM_HOME_RELEASE));
        assert(physical[a] > 0);
    }
    puts(z ? "PASS HOME Z paired phase barriers" : "PASS HOME X seek/backoff/latch/release");
}

static void home_faults(void) {
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X));
    arm_motion_tick(&s,&in,ARM_HOME_TIMEOUT_MS,&out);
    assert(s.fault == ARM_FAULT_HOME_TIMEOUT && !s.enabled && !s.axis[2].homed);
    reset(); in.axis[2].min_limit_active=true;
    assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X)); tick();
    arm_motion_on_steps(&s,ARM_AXIS_X,(int64_t)11000 * ARM_X_DRIVER_PULSES_PER_REV / ARM_X_UM_PER_REV); tick(); assert(s.fault == ARM_FAULT_HOME_SWITCH);
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X));
    in.axis[2].max_limit_active=true; tick(); assert(s.fault == ARM_FAULT_HOME_SWITCH);
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X)); tick();
    arm_motion_on_steps(&s,ARM_AXIS_X,-(int64_t)(ARM_X_TRAVEL_UM + 10000) * ARM_X_DRIVER_PULSES_PER_REV / ARM_X_UM_PER_REV); tick(); assert(s.fault == ARM_FAULT_HOME_SWITCH);
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_Z));
    arm_motion_on_steps(&s,ARM_AXIS_Z_LEFT,
        ((int64_t)ARM_Z_MAX_SKEW_UM + 1000) * ARM_DRIVER_PULSES_PER_REV / ARM_Z_UM_PER_REV);
    tick(); assert(s.fault == ARM_FAULT_Z_SKEW);
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X)); tick();
    arm_motion_command_stop(&s); tick(); assert(s.mode == ARM_MODE_IDLE && !s.axis[2].homed && out.axis[2].step_frequency_millihz==0);
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X)); tick();
    arm_motion_set_enabled(&s,false); tick(); assert(!s.axis[2].homed && s.mode == ARM_MODE_IDLE);
    puts("PASS timeout, stuck/opposite/missing switch, skew, STOP, disable");
}

static void bounce_and_limits(void) {
    reset(); assert(arm_motion_command_home(&s,ARM_VIRTUAL_AXIS_X)); tick();
    in.axis[2].min_limit_active=true; tick();
    assert(s.axis[2].home_phase==ARM_HOME_SEEK && s.axis[2].current_velocity_um_s==0);
    in.axis[2].min_limit_active=false; tick(); assert(s.axis[2].home_phase==ARM_HOME_SEEK);
    in.axis[2].min_limit_active=true; tick(); s.axis[2].pulse_active=true; tick();
    assert(s.axis[2].home_phase==ARM_HOME_SEEK);
    s.axis[2].pulse_active=false; tick(); assert(s.axis[2].home_phase==ARM_HOME_BACKOFF);
    reset(); in.axis[2].min_limit_active=true;
    assert(arm_motion_command_jog(&s,ARM_VIRTUAL_AXIS_X,1000)); tick(); assert(!s.faulted);
    assert(arm_motion_command_jog(&s,ARM_VIRTUAL_AXIS_X,-1000)); tick(); assert(s.fault==ARM_FAULT_HARD_LIMIT);
    reset(); in.axis[2].min_limit_active=in.axis[2].max_limit_active=true; tick(); assert(s.faulted);
    reset(); assert(arm_motion_set_axis_position(&s,ARM_AXIS_X,500000,true));
    assert(s.axis[0].soft_max_position_um == 850000);
    assert(s.axis[1].soft_max_position_um == 850000);
    assert(s.axis[2].soft_max_position_um == 950000);
    assert(!arm_motion_command_axis_move(&s,ARM_AXIS_X,950001,1000));
    assert(!arm_motion_set_axis_soft_limits(&s,ARM_AXIS_X,0,950001));
    assert(!arm_motion_command_axis_move(&s,ARM_AXIS_X,1000001,1000));
    assert(arm_motion_command_axis_move(&s,ARM_AXIS_X,500010,1000)); tick();
    assert(s.axis[2].position_um==500000);
    puts("PASS bounce, pulse barrier, directional limits, MOVE bounds/no position snap");
}

static void parser(void) {
    ArmDebugCommand c;
    const char *bad[]={"EN 2","EN 1 garbage","HB X","JOG X 123x","JOG X 99999999999999999999","HOME X junk","MOVE X 1 2 3","STOP now"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!arm_debug_parse_line(bad[i],&c));
    assert(arm_debug_parse_line("JOG ZL -1000",&c)); reset();
    s.axis[1].soft_max_position_um=100;
    assert(arm_debug_parse_line("SETPOS Z 1000",&c));
    assert(!arm_debug_apply_command(&s,&c)); assert(s.axis[0].position_um==0 && !s.axis[0].homed);
    puts("PASS strict parser and atomic grouped config");
}

static void planner(void) {
    ArmStepPlan p;
    const uint32_t frequencies[]={25,640000,1280000,256000,5000000};
    for (unsigned bits=16;bits<=32;bits+=16) for (unsigned i=0;i<5;i++) {
        assert(arm_step_plan(84000000,bits,frequencies[i],&p));
        uint64_t ticks=(uint64_t)p.autoreload+1, divisor=(uint64_t)p.prescaler+1;
        assert(ticks<=((uint64_t)1<<bits) && divisor<=65536);
        assert((ticks-p.compare)*divisor*1000000 >= 84000000ULL*ARM_STEP_HIGH_US);
        assert((uint64_t)p.compare*divisor*1000000 >= 84000000ULL*ARM_STEP_SETUP_US);
    }
    assert(!arm_step_plan(84000000,16,160000001,&p));
    assert(!arm_step_plan(84000000,16,0,&p)); assert(!arm_step_plan(84000000,8,1000,&p));
    puts("PASS timer ranges, minimum widths, frequency rejection");
}

static void soft_skew(void) {
    for (int sign=-1; sign<=1; sign+=2) {
        reset();
        for (int i=0;i<3;i++) assert(arm_motion_set_axis_position(&s,i,500000,true));
        int64_t fraction[2]={0};
        for (int n=0;n<10000;n++) {
            assert(arm_motion_command_axis_jog(&s,ARM_AXIS_Z_LEFT,sign*160000));
            assert(arm_motion_command_axis_jog(&s,ARM_AXIS_Z_RIGHT,-sign*160000));
            tick();
            for (int i=0;i<2;i++) {
                fraction[i]+=(int64_t)s.axis[i].current_velocity_um_s*10*ARM_AXIS_CONFIG[i].driver_pulses_per_rev;
                int64_t denominator=(int64_t)ARM_AXIS_CONFIG[i].um_per_motor_rev*1000;
                arm_motion_on_steps(&s,i,fraction[i]/denominator);
                fraction[i]%=denominator;
            }
            assert(!s.faulted && s.enabled);
            int64_t difference=(int64_t)s.axis[0].position_um-s.axis[1].position_um;
            assert(difference<=ARM_Z_MAX_SKEW_UM && difference>=-ARM_Z_MAX_SKEW_UM);
        }
        int32_t difference=sign*(s.axis[0].position_um-s.axis[1].position_um);
        assert(difference>ARM_Z_MAX_SKEW_UM-1000);
        assert(arm_motion_command_axis_jog(&s,ARM_AXIS_Z_LEFT,-sign*10000));
        assert(arm_motion_command_axis_jog(&s,ARM_AXIS_Z_RIGHT,sign*10000));
        tick();
        assert(sign*s.axis[0].current_velocity_um_s<0);
        assert(!s.faulted && s.enabled);
    }
    puts("PASS soft skew: both directions, braking at limit, retreat without disable");
}

static void level_lower_recovery(void) {
    for (int stuck=0; stuck<2; ++stuck) for (int side=0; side<2; ++side) {
        reset();
        assert(arm_motion_set_axis_position(&s, side, 0, false));
        assert(arm_motion_set_axis_position(&s, 1-side, 10000, false));
        assert(arm_motion_command_level(&s, side == 1));
        int64_t fraction[2] = {0};
        bool lifted = false;
        for (int n=0; n<3000 && s.leveling && !s.faulted; ++n) {
            for (int i=0; i<2; ++i)
                in.axis[i].min_limit_active = s.axis[i].position_um <= 0 || (stuck && i==side);
            int32_t difference = s.axis[0].position_um - s.axis[1].position_um;
            in.level_active = difference >= -200 && difference <= 200;
            tick();
            lifted |= s.level_phase == ARM_LEVEL_LIFT;
            for (int i=0; i<2; ++i) {
                assert(!in.axis[i].min_limit_active || s.axis[i].current_velocity_um_s >= 0);
                fraction[i] += (int64_t)s.axis[i].current_velocity_um_s * 10 * ARM_DRIVER_PULSES_PER_REV;
                int64_t denominator = (int64_t)ARM_Z_UM_PER_REV * 1000;
                arm_motion_on_steps(&s, i, fraction[i] / denominator);
                fraction[i] %= denominator;
            }
        }
        assert(lifted);
        if (stuck) assert(s.fault == ARM_FAULT_HOME_SWITCH && !s.enabled);
        else {
            assert(!s.leveling && !s.faulted && s.enabled);
            assert(s.axis[0].position_um > 19000 && s.axis[1].position_um > 19000);
            assert(s.axis[2].position_um == 0);
            assert(!s.axis[0].homed && !s.axis[1].homed);
        }
    }
    puts("PASS leveling: lower switch recovery on either Z, resume centering, stuck switch fault");
}

int main(void) {
    level_lower_recovery();
    reset();
    assert(ARM_X_DRIVER_PULSES_PER_REV == 3200);
    arm_motion_on_steps(&s, ARM_AXIS_X, 114860);
    assert(s.axis[2].position_um >= 898980 && s.axis[2].position_um <= 899020);
    for (int sign = -1; sign <= 1; sign += 2) {
        reset();
        assert(arm_motion_set_axis_position(&s, ARM_AXIS_X, 475000, true));
        int64_t fraction = 0;
        for (int n = 0; n < 10000; ++n) {
            assert(arm_motion_command_axis_jog(&s, ARM_AXIS_X, sign * 200000));
            tick();
            fraction += (int64_t)s.axis[2].current_velocity_um_s * 10 * ARM_X_DRIVER_PULSES_PER_REV;
            int64_t denominator = (int64_t)ARM_X_UM_PER_REV * 1000;
            arm_motion_on_steps(&s, ARM_AXIS_X, fraction / denominator);
            fraction %= denominator;
            assert(!s.faulted && s.enabled);
            assert(s.axis[2].position_um >= 0 && s.axis[2].position_um <= ARM_X_TRAVEL_UM);
        }
        assert(s.axis[2].current_velocity_um_s == 0);
        assert(sign < 0 ? s.axis[2].position_um < 100 : s.axis[2].position_um > ARM_X_TRAVEL_UM - 100);
        assert(arm_motion_command_axis_jog(&s, ARM_AXIS_X, -sign * 10000));
        tick();
        assert(sign * s.axis[2].current_velocity_um_s < 0 && !s.faulted);
    }
    puts("PASS X: braking at both soft limits, holding jog, retreat without fault");
    soft_skew();
    counting(); planner(); parser(); bounce_and_limits(); home_faults();
    home_sim(false,false,false); home_sim(false,true,false);
    home_sim(true,false,true); home_sim(true,true,true);
    puts("All motion/STEP planner checks passed."); return 0;
}
