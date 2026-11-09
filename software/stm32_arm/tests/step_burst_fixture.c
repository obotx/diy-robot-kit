#ifdef NDEBUG
#undef NDEBUG
#endif
#include "main.h"
#include "arm_step_burst.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
TIM_TypeDef mock_tim2, mock_tim3, mock_tim5;
GPIO_TypeDef mock_gpio;
DMA_Stream_TypeDef mock_dma_streams[3];
DMA_TypeDef mock_dma;
static uint32_t now;
uint32_t HAL_GetTick(void) { return now; }

static void reset(void) {
    arm_step_burst_abort();
    memset(&mock_tim2, 0, sizeof(mock_tim2));
    memset(&mock_tim3, 0, sizeof(mock_tim3));
    memset(&mock_tim5, 0, sizeof(mock_tim5));
    memset(mock_dma_streams, 0, sizeof(mock_dma_streams));
    memset(&mock_dma, 0, sizeof(mock_dma));
    arm_step_burst_init();
}

int main(void) {
    const uint32_t rates[] = {1000, 64000, 128000, 160000};
    const unsigned counts[] = {1, 2, 8192};
    for (int axis = 0; axis < 3; ++axis) for (unsigned f=0; f<4; ++f) for (unsigned n=0; n<3; ++n) {
        reset();
        assert(arm_step_burst_start(axis, rates[f], counts[n]));
        TIM_TypeDef *t = axis == 0 ? TIM2 : (axis == 1 ? TIM5 : TIM3);
        DMA_Stream_TypeDef *d = axis == 0 ? DMA1_Stream1 : (axis == 1 ? DMA1_Stream0 : DMA1_Stream2);
        unsigned shift = axis == 0 ? 6 : (axis == 1 ? 0 : 16);
        unsigned channel = axis == 0 ? 3 : (axis == 1 ? 6 : 5);
        assert((d->CR >> DMA_SxCR_CHSEL_Pos) == channel);
        assert(!(t->CR1 & TIM_CR1_CEN));
        assert(!arm_step_burst_start(axis, rates[f], counts[n]));
        ++now;
        arm_step_burst_poll();
        assert(t->CR1 & TIM_CR1_CEN);
        assert(t->DIER == TIM_DIER_UDE); /* no interrupt on each pulse */
        uint32_t compare = axis == 1 ? t->CCR2 : t->CCR1;
        uint64_t divisor = (uint64_t)t->PSC + 1;
        assert(((uint64_t)t->ARR + 1 - compare) * divisor * 1000000 >= 84000000ULL * 3);
        assert((uint64_t)compare * divisor * 1000000 >= 84000000ULL * 3);
        uint32_t *buffer = (uint32_t *)d->M0AR;
        volatile uint32_t *peripheral = (volatile uint32_t *)d->PAR;
        assert(peripheral == &t->CR1);
        /* Model update-request transfers with no firmware poll/IRQ between edges. */
        for (unsigned i=0; i<counts[n]; ++i) {
            assert(t->CR1 & TIM_CR1_CEN);
            t->CNT = 0;
            *peripheral = buffer[i];
            --d->NDTR;
        }
        assert(!(t->CR1 & TIM_CR1_CEN) && !d->NDTR);
        DMA1->LISR = 0x20U << shift;
        arm_step_burst_poll();
        assert(arm_step_burst_status().state == ARM_BURST_DONE);
    }
    reset();
    assert(!arm_step_burst_start(3, 128000, 100));
    assert(!arm_step_burst_start(0, 160001, 100));
    assert(!arm_step_burst_start(0, 999, 100));
    assert(!arm_step_burst_start(0, 128000, 0));
    assert(!arm_step_burst_start(0, 128000, 8193));
    assert(arm_step_burst_start(0, 128000, 100));
    ++now; arm_step_burst_poll();
    arm_step_burst_abort();
    assert(arm_step_burst_status().state == ARM_BURST_ABORTED && !TIM2->CR1 && !TIM2->CCER);
    reset();
    assert(arm_step_burst_start(0, 128000, 100));
    DMA1->LISR = 0x08U << 6;
    arm_step_burst_poll();
    assert(arm_step_burst_status().state == ARM_BURST_ERROR && !TIM2->CR1);
    reset();
    assert(arm_step_burst_start(0, 128000, 100));
    now += 200; arm_step_burst_poll();
    assert(arm_step_burst_status().state == ARM_BURST_ERROR && !TIM2->CR1);
    puts("PASS DMA burst: 3 axes, 1/2/8192 pulses, 1/64/128/160 kHz, widths, delayed CPU, range, STOP, DMA error, timeout");
}
