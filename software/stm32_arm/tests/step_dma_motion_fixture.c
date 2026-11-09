#ifdef NDEBUG
#undef NDEBUG
#endif
#include "main.h"
#include "arm_step_hw.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
TIM_TypeDef mock_tim2, mock_tim3, mock_tim5;
GPIO_TypeDef mock_gpio;
DMA_Stream_TypeDef mock_dma_streams[3];
DMA_TypeDef mock_dma;
static uint32_t ticks;
uint32_t HAL_GetTick(void) { return ticks; }
static bool allowed = true;
bool arm_board_pulse_allowed(ArmAxisId axis, bool positive) { (void)axis; (void)positive; return allowed; }
static TIM_TypeDef *timers[] = {TIM2,TIM5,TIM3};
static unsigned streams[] = {1,0,2}, shifts[] = {6,0,16};
static unsigned offset[3];
void mock_dma_start_check(void) {
    for (int a=0;a<3;a++) {
        DMA_Stream_TypeDef *d=&mock_dma_streams[streams[a]];
        if (!d->NDTR || (d->CR & DMA_SxCR_EN)) continue;
        /* At DMA enable, UG must not have the previous block's UDE active. */
        assert(timers[a]->DIER == 0);
        assert(timers[a]->CR1 == TIM_CR1_URS);
    }
}
static void edge(int a) {
    TIM_TypeDef *t = timers[a];
    DMA_Stream_TypeDef *d = &mock_dma_streams[streams[a]];
    assert(t->CR1 & TIM_CR1_CEN);
    assert(t->DIER == TIM_DIER_UDE && d->NDTR);
    t->CNT = 0; t->SR |= TIM_SR_UIF;
    *(volatile uint32_t *)d->PAR = ((uint32_t *)d->M0AR)[offset[a]++];
    if (!--d->NDTR) { d->CR &= ~DMA_SxCR_EN; mock_dma.LISR |= 0x20U << shifts[a]; }
}
static void irq(int a) {
    arm_step_hw_dma_irq((ArmAxisId)a);
    mock_dma.LISR &= ~mock_dma.LIFCR;
    mock_dma.LIFCR = 0;
    offset[a] = 0;
}
static void reset(void) {
    arm_step_hw_abort();
    memset(mock_dma_streams,0,sizeof(mock_dma_streams)); memset(&mock_dma,0,sizeof(mock_dma));
    memset(offset,0,sizeof(offset)); ticks += 10; allowed = true; arm_step_hw_init();
}
int main(void) {
    ArmMotionState state;
    ArmMotionOutputs out;
    /* Cross the real 7/8 mm/s threshold and restart several finite blocks. */
    reset(); arm_motion_init(&state); memset(&out,0,sizeof(out));
    out.axis[0]=(ArmAxisOutput){true,true,4480000};
    assert(arm_step_hw_apply(&out));
    TIM2->CNT=0; TIM2->SR=TIM_SR_UIF; TIM2->CR1 &= ~TIM_CR1_CEN;
    ticks+=2;
    out.axis[0].step_frequency_millihz=5120000;
    assert(arm_step_hw_apply(&out));
    for (int block=0;block<4;block++) {
        assert(DMA1_Stream1->NDTR==5);
        while (DMA1_Stream1->NDTR) edge(0);
        irq(0);
    }
    out.axis[0].step_frequency_millihz=4480000;
    assert(arm_step_hw_apply(&out));
    while (DMA1_Stream1->NDTR) edge(0);
    irq(0);
    assert(TIM2->CR1 & TIM_CR1_OPM);
    arm_step_hw_feedback(&state);
    assert(!state.faulted && state.axis[0].position_numerator==26LL*5000);
    arm_step_hw_abort();
    /* Three axes concurrently, exact finite budgets and CPU-delayed final IRQ. */
    for (unsigned rate=64000; rate<=160000; rate+=32000) {
        reset(); arm_motion_init(&state); memset(&out,0,sizeof(out));
        for (int a=0;a<3;a++) {
            assert(arm_step_hw_begin_limit((ArmAxisId)a,323,a!=1));
            out.axis[a]=(ArmAxisOutput){true,a!=1,rate*1000};
        }
        assert(arm_step_hw_apply(&out));
        assert(!(TIM2->CR1 & TIM_CR1_CEN));
        ticks+=2; assert(arm_step_hw_apply(&out));
        unsigned total[3]={0};
        while (!arm_step_hw_limit_done(ARM_AXIS_Z_LEFT)) {
            for (int a=0;a<3;a++) {
                DMA_Stream_TypeDef *d=&mock_dma_streams[streams[a]];
                if (!d->NDTR) continue;
                uint32_t high=timers[a]->ARR+1-(a==1?timers[a]->CCR2:timers[a]->CCR1);
                assert(high>=252 && timers[a]->ARR+1-high>=252);
                while (d->NDTR) { edge(a); total[a]++; }
                assert(!(timers[a]->CR1 & TIM_CR1_CEN));
                arm_step_hw_feedback(&state);
                irq(a);
            }
        }
        arm_step_hw_feedback(&state);
        for (int a=0;a<3;a++) {
            assert(total[a]==323);
            assert(state.axis[a].position_numerator==(a==1?-323LL:323LL)*ARM_AXIS_CONFIG[a].um_per_motor_rev);
        }
    }
    /* DMA errors terminate all axes; no stale interrupt may restart. */
    reset(); arm_motion_init(&state); memset(&out,0,sizeof(out));
    out.axis[0]=(ArmAxisOutput){true,true,128000000};
    assert(arm_step_hw_apply(&out)); ticks+=2; assert(arm_step_hw_apply(&out));
    for (int i=0;i<10;i++) edge(0);
    arm_step_hw_feedback(&state); assert(state.axis[0].position_numerator==50000);
    out.axis[0]=(ArmAxisOutput){true,false,64000000};
    assert(arm_step_hw_apply(&out));
    assert(!(mock_gpio.pins & ZL_DIR_Pin));
    while (DMA1_Stream1->NDTR) edge(0);
    irq(0); assert(!(TIM2->CR1 & TIM_CR1_CEN));
    assert(mock_gpio.pins & ZL_DIR_Pin);
    ticks+=2; assert(arm_step_hw_apply(&out));
    assert(DMA1_Stream1->NDTR==64);
    while (DMA1_Stream1->NDTR) edge(0);
    out.axis[0].step_frequency_millihz=1000000;
    assert(arm_step_hw_apply(&out));
    assert(TIM2->CR1 & TIM_CR1_OPM);
    arm_step_hw_feedback(&state); assert(state.axis[0].position_numerator==64LL*5000);
    /* Foreground switched to OPM before the pending DMA completion ISR.
     * The old TC flag must be acknowledged, without touching the OPM pulse. */
    assert(mock_dma.LISR & (0x20U << shifts[0]));
    uint32_t opm_cr1 = TIM2->CR1;
    mock_dma.LIFCR = 0; /* Earlier write-to-clear commands are already consumed. */
    irq(0);
    assert(!(mock_dma.LISR & (0x3DU << shifts[0])));
    assert(TIM2->CR1 == opm_cr1);
    arm_step_hw_feedback(&state);
    assert(!state.faulted && state.axis[0].position_numerator==64LL*5000);
    arm_step_hw_abort();

    reset(); arm_motion_init(&state); memset(&out,0,sizeof(out));
    out.axis[0]=(ArmAxisOutput){true,true,128000000};
    assert(arm_step_hw_apply(&out)); ticks+=2; assert(arm_step_hw_apply(&out));
    mock_dma.LISR=0x08U<<6; irq(0); arm_step_hw_feedback(&state);
    assert(state.faulted && !(TIM2->CR1 & TIM_CR1_CEN));
    irq(0); assert(!(TIM2->CR1 & TIM_CR1_CEN));
    /* Target/soft-limit budget clips blocks even if the CPU stops polling. */
    reset(); arm_motion_init(&state); state.axis[0].homed=true;
    state.axis[0].target_position_active=true; state.axis[0].target_position_um=50;
    arm_step_hw_feedback(&state);
    assert(arm_step_hw_apply(&out)); ticks+=2; assert(arm_step_hw_apply(&out));
    assert(DMA1_Stream1->NDTR==32);
    while (DMA1_Stream1->NDTR) edge(0);
    irq(0); assert(!(TIM2->CR1 & TIM_CR1_CEN));
    arm_step_hw_feedback(&state); assert(state.axis[0].position_um==50);
    /* Raw guard and emergency STOP stop the DMA source, not only timer CEN. */
    reset(); assert(arm_step_hw_apply(&out)); ticks+=2; assert(arm_step_hw_apply(&out));
    allowed=false; arm_step_hw_guard();
    assert(!(DMA1_Stream1->CR & DMA_SxCR_EN) && !(TIM2->CR1 & TIM_CR1_CEN));
    arm_step_hw_abort(); irq(0); assert(!(TIM2->CR1 & TIM_CR1_CEN));
    puts("PASS motion DMA: concurrent axes 64/96/128/160 kHz, exact counts, target bounds, DMA error, guard, abort");
}
