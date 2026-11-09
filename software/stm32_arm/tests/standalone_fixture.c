#ifdef NDEBUG
#undef NDEBUG
#endif
#include "main.h"
#include "arm_step_hw.h"
#include "arm_debug_protocol.h"
#include <assert.h>
#include <stdio.h>
TIM_TypeDef mock_tim2, mock_tim3, mock_tim5;
GPIO_TypeDef mock_gpio;
DMA_Stream_TypeDef mock_dma_streams[3];
DMA_TypeDef mock_dma;
static uint32_t ticks = 100;
uint32_t HAL_GetTick(void) { return ticks; }
bool arm_board_pulse_allowed(ArmAxisId axis, bool positive) { (void)axis; (void)positive; return true; }
static void pulse(void) {
    assert(TIM2->CR1 & TIM_CR1_CEN);
    TIM2->CNT = 0;
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->SR |= TIM_SR_UIF;
    arm_step_hw_irq(ARM_AXIS_Z_LEFT);
}
static void test_home(int scenario, int32_t start_um) {
    ArmMotionState s;
    ArmMotionOutputs out = {0};
    ArmMotionInputs in = {.motor_power_enabled=true, .e_stop_ok=true};
    arm_motion_init(&s);
    arm_motion_set_enabled(&s, true);
    assert(!arm_motion_command_home(&s, ARM_VIRTUAL_AXIS_Z));
    assert(arm_motion_command_home_zl(&s));
    int64_t physical = (int64_t)start_um * ARM_DRIVER_PULSES_PER_REV;
    int64_t remainder = 0;
    for (uint32_t tick=0; tick<ARM_ZL_HOME_TIMEOUT_MS/10+100 && !s.faulted && s.mode==ARM_MODE_HOMING; ++tick) {
        in.axis[0].min_limit_active = scenario == 2 || (scenario != 1 && physical <= 0);
        in.axis[0].max_limit_active = scenario == 3;
        arm_motion_tick(&s, &in, 10, &out);
        assert(!out.axis[1].step_frequency_millihz && !out.axis[2].step_frequency_millihz);
        assert(s.axis[0].current_velocity_um_s >= -5000 && s.axis[0].current_velocity_um_s <= 1000);
        if (s.mode != ARM_MODE_HOMING) break;
        remainder += (int64_t)s.axis[0].current_velocity_um_s * 10 * ARM_DRIVER_PULSES_PER_REV;
        int64_t steps = remainder / (1000LL * ARM_Z_UM_PER_REV);
        remainder %= 1000LL * ARM_Z_UM_PER_REV;
        arm_motion_on_steps(&s, ARM_AXIS_Z_LEFT, steps);
        physical += steps * ARM_Z_UM_PER_REV;
    }
    if (!scenario) {
        assert(!s.faulted && s.mode == ARM_MODE_IDLE && s.axis[0].homed);
        assert(s.axis[0].position_um == 0 && physical > 0);
        assert(!s.axis[1].homed && !s.axis[2].homed);
    } else assert(s.faulted && !s.axis[0].homed && s.fault == ARM_FAULT_HOME_SWITCH);
}
int main(void) {
    ArmMotionState s;
    ArmMotionOutputs out = {0};
    ArmMotionInputs in = {.motor_power_enabled=true, .e_stop_ok=true};
    ArmDebugCommand cmd;
    assert(ARM_Z_MAX_SKEW_UM == 100000);
    arm_motion_init(&s);
    arm_motion_set_enabled(&s, true);
    assert(!arm_motion_command_jog(&s, ARM_VIRTUAL_AXIS_Z, 1000));
    assert(!arm_motion_command_axis_jog(&s, ARM_AXIS_Z_RIGHT, 1000));
    assert(!arm_motion_command_axis_move(&s, ARM_AXIS_Z_LEFT, 20000, 1000));
    assert(arm_debug_parse_line("TRAVEL ZL -20000 1000", &cmd));
    assert(cmd.type == ARM_DEBUG_CMD_TRAVEL && cmd.position_um == -20000);
    assert(!arm_debug_apply_command(&s, &cmd));
    assert(arm_debug_parse_line("HOME ZL", &cmd));
    assert(!arm_debug_parse_line("HOME ZR", &cmd));
    for (int scenario=0; scenario<4; ++scenario) test_home(scenario, 30000);
    test_home(0, 500000);
    test_home(0, ARM_Z_TRAVEL_UM);
    ArmMotionState timeout;
    arm_motion_init(&timeout);
    arm_motion_set_enabled(&timeout, true);
    assert(arm_motion_command_home_zl(&timeout));
    arm_motion_tick(&timeout, &in, ARM_ZL_HOME_TIMEOUT_MS, &out);
    assert(timeout.fault == ARM_FAULT_HOME_TIMEOUT);
    arm_motion_on_steps(&s, ARM_AXIS_Z_LEFT, 64000);
    arm_motion_tick(&s, &in, 10, &out);
    assert(!s.faulted && s.axis[0].position_um == 100000);
    arm_motion_on_step(&s, ARM_AXIS_Z_LEFT, true);
    arm_motion_tick(&s, &in, 10, &out);
    assert(s.fault == ARM_FAULT_Z_SKEW);
    arm_motion_init(&s);
    arm_motion_on_steps(&s, ARM_AXIS_Z_LEFT, -64001);
    arm_motion_tick(&s, &in, 10, &out);
    assert(s.fault == ARM_FAULT_Z_SKEW);

    arm_motion_init(&s);
    arm_step_hw_init();
    for (int i=0; i<3; ++i) out.axis[i] = (ArmAxisOutput){true,true,3200000};
    assert(arm_step_hw_apply(&out));
    assert(!(TIM2->CR1 & TIM_CR1_CEN) && !(TIM5->CR1 & TIM_CR1_CEN) && !(TIM3->CR1 & TIM_CR1_CEN));
    arm_step_hw_abort();
    assert(!arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 12801, true));
    assert(arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 12800, true));
    assert(arm_step_hw_apply(&out));
    assert(!arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 12800, true));
    assert(!(mock_gpio.pins & ZL_DIR_Pin));
    for (int i=0; i<12800; ++i) pulse();
    assert(arm_step_hw_limit_done(ARM_AXIS_Z_LEFT));
    assert(arm_step_hw_apply(&out));
    assert(!(TIM2->CR1 & TIM_CR1_CEN));
    assert(!(TIM5->CR1 & TIM_CR1_CEN) && !(TIM3->CR1 & TIM_CR1_CEN));
    arm_step_hw_feedback(&s);
    assert(s.axis[0].position_um == 20000 && s.axis[1].position_um == 0);
    arm_step_hw_abort();
    assert(arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 12800, false));
    assert(arm_step_hw_apply(&out)); /* Wrong direction cannot spend the budget. */
    assert(!(TIM2->CR1 & TIM_CR1_CEN));
    out.axis[0].dir_positive = false;
    assert(arm_step_hw_apply(&out));
    assert(mock_gpio.pins & ZL_DIR_Pin);
    for (int i=0; i<12800; ++i) pulse();
    arm_step_hw_feedback(&s);
    assert(s.axis[0].position_um == 0 && arm_step_hw_limit_done(ARM_AXIS_Z_LEFT));
    arm_step_hw_abort();
    assert(arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 100, false));
    assert(arm_step_hw_apply(&out));
    pulse();
    arm_step_hw_abort();
    assert(arm_step_hw_apply(&out));
    assert(!(TIM2->CR1 & TIM_CR1_CEN));
    arm_step_hw_feedback(&s);
    arm_step_hw_abort();
    assert(arm_step_hw_begin_home_limit());
    out.axis[0] = (ArmAxisOutput){true,false,640000};
    assert(arm_step_hw_apply(&out));
    const uint32_t home_pulses = (uint64_t)ARM_ZL_HOME_TOTAL_MAX_UM * ARM_DRIVER_PULSES_PER_REV / ARM_Z_UM_PER_REV;
    for (uint32_t i=0; i<home_pulses/2; ++i) pulse();
    out.axis[0].dir_positive = true;
    assert(arm_step_hw_apply(&out));
    for (uint32_t i=home_pulses/2; i<home_pulses; ++i) pulse();
    assert(arm_step_hw_limit_done(ARM_AXIS_Z_LEFT));
    assert(arm_step_hw_apply(&out));
    assert(!(TIM2->CR1 & TIM_CR1_CEN));
    puts("PASS standalone homing: ZL only, missing/stuck/opposite switch, timeout, total bidirectional budget");
    puts("PASS standalone: 100mm skew, isolated ZL, exact +/-20mm budget, abort cannot resume");
    arm_step_hw_abort(); arm_motion_init(&s); arm_step_hw_feedback(&s);
    assert(arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 12800, true));
    for (int i=0;i<3;i++) out.axis[i]=(ArmAxisOutput){true,true,128000000};
    assert(arm_step_hw_apply(&out)); ticks+=2; assert(arm_step_hw_apply(&out));
    unsigned pulses=0;
    while (!arm_step_hw_limit_done(ARM_AXIS_Z_LEFT)) {
        unsigned index=0;
        assert(DMA1_Stream1->NDTR);
        while (DMA1_Stream1->NDTR) {
            assert(TIM2->CR1 & TIM_CR1_CEN);
            TIM2->CNT=0;
            TIM2->CR1=((uint32_t *)DMA1_Stream1->M0AR)[index++];
            DMA1_Stream1->NDTR--; pulses++;
        }
        DMA1_Stream1->CR &= ~DMA_SxCR_EN;
        mock_dma.LISR=0x20U<<6;
        arm_step_hw_dma_irq(ARM_AXIS_Z_LEFT);
        mock_dma.LISR=0;
    }
    arm_step_hw_feedback(&s);
    assert(pulses==12800 && s.axis[0].position_um==20000);
    assert(!(TIM5->CR1 & TIM_CR1_CEN) && !(TIM3->CR1 & TIM_CR1_CEN));
    puts("PASS standalone motion at 128 kHz: exact 20 mm, ZR/X remain blocked");
    return 0;
}
