#include "main.h"
#include "arm_step_burst.h"
#include "arm_step_plan.h"

/* Isolated diagnostic backend: no motor enables, no motion position accounting.
 * RM0390 table 28: TIM2_UP=S1/C3, TIM5_UP=S0/C6, TIM3_UP=S2/C5.
 * Update DMA writes CR1=CEN after each PWM2 falling edge, and CR1=0 after
 * the final edge. Normal DMA mode (never circular) bounds each pulse train.
 * Verify DMA latency/last edge on hardware before using this for motion.
 */
static uint32_t sequence[ARM_BURST_MAX_PULSES];
static TIM_TypeDef *timer;
static DMA_Stream_TypeDef *stream;
static uint32_t flag_shift, started, clock_hz;
static ArmBurstStatus status;

static uint32_t lock(void) { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static bool busy(void) { return status.state == ARM_BURST_SETUP || status.state == ARM_BURST_RUNNING; }

static void stop(void)
{
    if (!timer) return;
    /* Disable requests before disabling the stream; any pending DMA write may
     * still restart CR1, so stop the timer once more after stream shutdown. */
    timer->DIER = 0;
    timer->CR1 = 0;
    stream->CR &= ~DMA_SxCR_EN;
    while (stream->CR & DMA_SxCR_EN) {}
    timer->CR1 = 0;
    timer->CCER = 0;
    DMA1->LIFCR = 0x3DU << flag_shift;
}

void arm_step_burst_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM5_CLK_ENABLE();
    RCC_ClkInitTypeDef clock;
    uint32_t latency;
    HAL_RCC_GetClockConfig(&clock, &latency);
    clock_hz = HAL_RCC_GetPCLK1Freq() * (clock.APB1CLKDivider == RCC_HCLK_DIV1 ? 1U : 2U);
    status.state = ARM_BURST_IDLE;
}

bool arm_step_burst_start(ArmAxisId axis, uint32_t hz, uint32_t pulses)
{
    ArmStepPlan plan;
    if ((unsigned)axis >= ARM_AXIS_COUNT || hz < 1000 || hz > ARM_BURST_MAX_HZ ||
        !pulses || pulses > ARM_BURST_MAX_PULSES ||
        !arm_step_plan_limits(clock_hz, axis == ARM_AXIS_X ? 16 : 32, hz * 1000U,
                              ARM_BURST_MAX_HZ, 3, 3, &plan)) return false;
    uint32_t irq = lock();
    if (busy()) { __set_PRIMASK(irq); return false; }
    stop();
    timer = axis == ARM_AXIS_Z_LEFT ? TIM2 : (axis == ARM_AXIS_Z_RIGHT ? TIM5 : TIM3);
    stream = axis == ARM_AXIS_Z_LEFT ? DMA1_Stream1 : (axis == ARM_AXIS_Z_RIGHT ? DMA1_Stream0 : DMA1_Stream2);
    flag_shift = axis == ARM_AXIS_Z_LEFT ? 6U : (axis == ARM_AXIS_Z_RIGHT ? 0U : 16U);
    stop();
    for (uint32_t i = 0; i < pulses; ++i) sequence[i] = i + 1U == pulses ? 0U : TIM_CR1_CEN;
    timer->SMCR = 0;
    timer->CR2 = 0;
    timer->PSC = plan.prescaler;
    timer->ARR = plan.autoreload;
    timer->CCMR1 = axis == ARM_AXIS_Z_RIGHT ? (7U << TIM_CCMR1_OC2M_Pos) : (7U << TIM_CCMR1_OC1M_Pos);
    if (axis == ARM_AXIS_Z_RIGHT) timer->CCR2 = plan.compare;
    else timer->CCR1 = plan.compare;
    timer->EGR = TIM_EGR_UG;
    timer->CNT = 0;
    timer->SR = 0;
    stream->PAR = (uintptr_t)&timer->CR1;
    stream->M0AR = (uintptr_t)sequence;
    stream->NDTR = pulses;
    stream->FCR = 0;
    uint32_t channel = axis == ARM_AXIS_Z_LEFT ? 3U : (axis == ARM_AXIS_Z_RIGHT ? 6U : 5U);
    stream->CR = (channel << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_DIR_0 | DMA_SxCR_MINC |
                 DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_PL;
    HAL_GPIO_WritePin(axis == ARM_AXIS_Z_LEFT ? ZL_DIR_GPIO_Port :
                     (axis == ARM_AXIS_Z_RIGHT ? ZR_DIR_GPIO_Port : X_DIR_GPIO_Port),
                     axis == ARM_AXIS_Z_LEFT ? ZL_DIR_Pin : (axis == ARM_AXIS_Z_RIGHT ? ZR_DIR_Pin : X_DIR_Pin),
                     GPIO_PIN_RESET);
    __DMB();
    stream->CR |= DMA_SxCR_EN;
    timer->CCER = axis == ARM_AXIS_Z_RIGHT ? TIM_CCER_CC2E : TIM_CCER_CC1E;
    timer->DIER = TIM_DIER_UDE; /* No per-pulse interrupt. */
    started = HAL_GetTick();
    status = (ArmBurstStatus){ARM_BURST_SETUP, axis, hz, pulses};
    __set_PRIMASK(irq);
    return true;
}

void arm_step_burst_poll(void)
{
    uint32_t irq = lock();
    if (busy()) {
        uint32_t flags = DMA1->LISR >> flag_shift;
        uint32_t elapsed = HAL_GetTick() - started;
        if ((flags & 0x0DU) || elapsed > (status.pulses * 1000U + status.hz - 1U) / status.hz + 100U) {
            stop();
            status.state = ARM_BURST_ERROR;
        } else if (status.state == ARM_BURST_SETUP && elapsed >= 1U) {
            timer->CR1 = TIM_CR1_CEN; /* DIR setup only once per train, >=1 ms. */
            status.state = ARM_BURST_RUNNING;
        } else if (status.state == ARM_BURST_RUNNING && (flags & 0x20U) && !stream->NDTR) {
            bool ended = !(timer->CR1 & TIM_CR1_CEN);
            stop();
            status.state = ended ? ARM_BURST_DONE : ARM_BURST_ERROR;
        }
    }
    __set_PRIMASK(irq);
}

void arm_step_burst_abort(void)
{
    uint32_t irq = lock();
    if (busy()) { stop(); status.state = ARM_BURST_ABORTED; }
    __set_PRIMASK(irq);
}

ArmBurstStatus arm_step_burst_status(void)
{
    uint32_t irq = lock();
    ArmBurstStatus result = status;
    __set_PRIMASK(irq);
    return result;
}
