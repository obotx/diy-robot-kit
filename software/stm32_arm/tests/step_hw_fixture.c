#ifdef NDEBUG
#undef NDEBUG
#endif
#include "main.h"
#include "arm_step_hw.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
TIM_TypeDef mock_tim2,mock_tim3,mock_tim5;
GPIO_TypeDef mock_gpio;
DMA_Stream_TypeDef mock_dma_streams[3];
DMA_TypeDef mock_dma;
uint32_t HAL_GetTick(void) { return 100; }
static bool allowed[3] = {true,true,true};
static bool inject_overflow;
bool arm_board_pulse_allowed(ArmAxisId a,bool positive) { (void)positive; return allowed[a]; }
static void overflow(TIM_TypeDef *t) {
    assert(t->CR1 & TIM_CR1_CEN);
    assert(t->CR1 & TIM_CR1_OPM);
    t->CNT=0; t->CR1&=~TIM_CR1_CEN; t->SR|=TIM_SR_UIF;
}
void arm_step_account_check(TIM_TypeDef *timer) {
    if (inject_overflow && timer == TIM2) {
        inject_overflow = false;
        overflow(timer);
    }
}
int main(void) {
    ArmMotionState state; arm_motion_init(&state);
    ArmMotionOutputs out={0}; arm_step_hw_init();
    out.axis[0]=(ArmAxisOutput){true,true,640000};
    out.axis[1]=(ArmAxisOutput){true,false,1280000};
    out.axis[2]=(ArmAxisOutput){true,true,256000};
    assert(arm_step_hw_apply(&out));
    assert(TIM2->ARR != TIM5->ARR);
    assert(!(mock_gpio.pins & ZL_DIR_Pin));
    assert(mock_gpio.pins & ZR_DIR_Pin);
    assert(mock_gpio.pins & X_DIR_Pin);
    uint32_t zr_period=TIM5->ARR;
    /* Hardware can overflow between the foreground SR read and flag clear,
     * even while CPU interrupts are masked. Losing UIF leaves running=true
     * with CEN=0, so HOME never schedules another pulse. */
    inject_overflow=true;
    assert(arm_step_hw_apply(&out));
    arm_step_hw_irq(ARM_AXIS_Z_LEFT);
    assert(TIM2->CR1 & TIM_CR1_CEN);
    arm_step_hw_feedback(&state);
    assert(state.axis[0].position_numerator==5000);
    arm_motion_init(&state);
    TIM2->CNT=100;
    assert(arm_step_hw_apply(&out) && TIM2->CNT==100);
    out.axis[0].step_frequency_millihz=320000;
    assert(arm_step_hw_apply(&out) && TIM5->ARR==zr_period);
    arm_step_hw_feedback(&state); assert(state.axis[0].position_um==0);
    overflow(TIM2);
    /* Hardware stops on update, even if the IRQ is delayed. */
    assert(!(TIM2->CR1&TIM_CR1_CEN));
    arm_step_hw_irq(ARM_AXIS_Z_LEFT); arm_step_hw_feedback(&state);
    assert(state.axis[0].position_numerator==5000);
    arm_step_hw_irq(ARM_AXIS_Z_LEFT); arm_step_hw_feedback(&state);
    assert(state.axis[0].position_numerator==5000);
    overflow(TIM5); arm_step_hw_irq(ARM_AXIS_Z_RIGHT); arm_step_hw_feedback(&state);
    assert(state.axis[1].position_numerator==-5000);
    /* Normal STOP preserves the high tail and accounts exactly one pulse. */
    TIM2->CNT=TIM2->CCR1;
    out.axis[0].step_frequency_millihz=0;
    assert(arm_step_hw_apply(&out) && (TIM2->CR1&TIM_CR1_CEN));
    overflow(TIM2); arm_step_hw_irq(ARM_AXIS_Z_LEFT); arm_step_hw_feedback(&state);
    assert(state.axis[0].position_numerator==10000 && !(TIM2->CR1&TIM_CR1_CEN));
    /* A limit asserted in the low part cancels it without counting a pulse. */
    allowed[2]=false; arm_step_hw_guard(); arm_step_hw_feedback(&state);
    assert(!state.axis[2].pulse_active && state.axis[2].position_um==0);
    allowed[2]=true; assert(arm_step_hw_apply(&out));
    /* Direction changes only after the old pulse has completed. */
    TIM3->CNT=TIM3->CCR1; out.axis[2].dir_positive=false;
    assert(arm_step_hw_apply(&out) && (mock_gpio.pins & X_DIR_Pin));
    overflow(TIM3); arm_step_hw_irq(ARM_AXIS_X); arm_step_hw_feedback(&state);
    assert(state.axis[2].position_numerator==ARM_X_UM_PER_REV && !(mock_gpio.pins & X_DIR_Pin));
    /* Pending UIF must be counted exactly once if abort beats the ISR. */
    overflow(TIM3); arm_step_hw_abort(); arm_step_hw_irq(ARM_AXIS_X); arm_step_hw_feedback(&state);
    assert(state.axis[2].position_numerator==0 && !state.faulted);
    assert(arm_step_hw_apply(&out)); TIM3->CNT=TIM3->CCR1;
    state.axis[2].homed=true; arm_step_hw_abort(); arm_step_hw_feedback(&state);
    assert(state.fault==ARM_FAULT_STEP_GENERATOR && !state.axis[2].homed);
    out.axis[0].step_frequency_millihz=160000001;
    assert(!arm_step_hw_apply(&out));
    puts("PASS timer-register mock: independent axes, OPM count, STOP, limit, DIR, abort races, rate limit");
    return 0;
}
