#include "main.h"
#include "arm_step_hw.h"
#include "arm_step_plan.h"
#include <string.h>

typedef struct {
    TIM_TypeDef *timer;
    GPIO_TypeDef *dir_port;
    uint16_t dir_pin;
    bool channel2;
    bool dir_inverted;
    bool running, positive, requested_positive;
    uint32_t requested_frequency;
    ArmStepPlan plan;
    int64_t completed;
    bool limited, limit_positive;
    bool bidirectional_limit;
    uint32_t remaining;
    DMA_Stream_TypeDef *dma;
    uint32_t dma_channel, dma_shift, batch, accounted;
    uint32_t sequence[160];
    bool dma_running, dir_known;
    uint32_t dir_tick;
    bool position_bounded;
    int64_t positive_budget, negative_budget;
} StepChannel;

static StepChannel channels[ARM_AXIS_COUNT];
static bool initialized, uncertain;
static uint32_t timer_hz;

static uint32_t lock(void) { uint32_t value = __get_PRIMASK(); __disable_irq(); return value; }
static uint32_t compare(const StepChannel *c) { return c->channel2 ? c->timer->CCR2 : c->timer->CCR1; }

static void account(StepChannel *c)
{
    if (c->dma_running) {
        uint32_t count = c->batch - c->dma->NDTR;
        uint32_t delta = count - c->accounted;
        c->completed += c->positive ? (int64_t)delta : -(int64_t)delta;
        if (c->limited) c->remaining -= delta;
        if (c->position_bounded) {
            c->positive_budget -= c->positive ? (int64_t)delta : -(int64_t)delta;
            c->negative_budget += c->positive ? (int64_t)delta : -(int64_t)delta;
        }
        c->accounted = count;
        if (!c->dma->NDTR && !(c->timer->CR1 & TIM_CR1_CEN)) c->running = false;
        return;
    }
    uint32_t status = c->timer->SR;
#ifdef ARM_STEP_ACCOUNT_CHECK
    arm_step_account_check(c->timer);
#endif
    if (c->running && (status & TIM_SR_UIF)) {
        c->completed += c->positive ? 1 : -1;
        if (c->position_bounded) {
            c->positive_budget -= c->positive ? 1 : -1;
            c->negative_budget += c->positive ? 1 : -1;
        }
        if (c->limited && c->remaining) --c->remaining;
        c->running = false;
        /* OPM has stopped: acknowledge only the completion we observed.
         * An unconditional clear can erase an update arriving after the SR
         * read, leaving software running while hardware CEN is already zero. */
        c->timer->SR = 0;
    } else if (!c->running && (status & TIM_SR_UIF)) {
        /* A stopped channel may retain a stale completion; acknowledge it
         * without counting it or leaving its interrupt pending. */
        c->timer->SR = 0;
    }
}

static void stop_dma(StepChannel *c)
{
    c->timer->DIER = 0;
    c->timer->CR1 = 0;
    c->dma->CR &= ~DMA_SxCR_EN;
    while (c->dma->CR & DMA_SxCR_EN) {}
    c->timer->CR1 = 0;
    account(c);
    if (c->timer->CNT >= compare(c) || (c->timer->SR & TIM_SR_UIF && c->accounted < c->batch)) uncertain = true;
    c->timer->CCER = 0;
    DMA1->LIFCR = 0x3DU << c->dma_shift;
    c->running = c->dma_running = false;
}

static void start(StepChannel *c, ArmAxisId axis)
{
    if (uncertain) return;
    if (ARM_JSON_ZL_ONLY && axis != ARM_AXIS_Z_LEFT) return;
    if (ARM_STANDALONE_ZL && (axis != ARM_AXIS_Z_LEFT || !c->limited)) return;
    if (c->limited && (!c->remaining || (!c->bidirectional_limit && c->requested_positive != c->limit_positive))) return;
    if (!c->requested_frequency || !arm_board_pulse_allowed(axis, c->requested_positive)) return;
    TIM_TypeDef *t = c->timer;
    bool fast = c->requested_frequency > ARM_STEP_OPM_MAX_HZ * 1000U;
    int64_t position_budget = c->requested_positive ? c->positive_budget : c->negative_budget;
    if (fast && c->position_bounded && position_budget <= 0) return;
    if (!c->dir_known || c->positive != c->requested_positive) {
        c->dir_known = true;
        c->dir_tick = HAL_GetTick();
        HAL_GPIO_WritePin(c->dir_port, c->dir_pin,
            (c->requested_positive != c->dir_inverted) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        c->positive = c->requested_positive;
    }
    /* Two tick boundaries guarantee at least one full millisecond. */
    if (fast && HAL_GetTick() - c->dir_tick < 2U) return;
    HAL_GPIO_WritePin(c->dir_port, c->dir_pin,
        (c->requested_positive != c->dir_inverted) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    c->positive = c->requested_positive;
    /* UG reloads PSC/ARR but must never request a DMA transfer. Disable the
     * old request source before UG, including when restarting a DMA block. */
    t->DIER = 0;
    t->CR1 = TIM_CR1_URS;
    t->PSC = c->plan.prescaler;
    t->ARR = c->plan.autoreload;
    if (c->channel2) t->CCR2 = c->plan.compare;
    else t->CCR1 = c->plan.compare;
    t->CNT = 0;
    t->EGR = TIM_EGR_UG;
    t->SR = 0;
    c->running = true;
    t->CCER = c->channel2 ? TIM_CCER_CC2E : TIM_CCER_CC1E;
    c->dma_running = fast;
    if (fast) {
        c->dma->CR &= ~DMA_SxCR_EN;
        while (c->dma->CR & DMA_SxCR_EN) {}
        DMA1->LIFCR = 0x3DU << c->dma_shift;
        /* At most 1 ms per finite block. The last update stops the timer
         * without CPU intervention; the completion IRQ schedules the next. */
        c->batch = c->requested_frequency / 1000000U;
        if (c->batch > 160U) c->batch = 160U;
        if (c->limited && c->batch > c->remaining) c->batch = c->remaining;
        if (c->position_bounded && (int64_t)c->batch > position_budget) c->batch = (uint32_t)position_budget;
        c->accounted = 0;
        for (uint32_t i = 0; i < c->batch; ++i)
            c->sequence[i] = TIM_CR1_URS | (i + 1U == c->batch ? 0U : TIM_CR1_CEN);
        c->dma->PAR = (uintptr_t)&t->CR1;
        c->dma->M0AR = (uintptr_t)c->sequence;
        c->dma->NDTR = c->batch;
        c->dma->FCR = 0;
        c->dma->CR = (c->dma_channel << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_DIR_0 | DMA_SxCR_MINC |
            DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_PL | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
        __DMB();
        c->dma->CR |= DMA_SxCR_EN;
        t->DIER = TIM_DIER_UDE;
        t->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
    } else {
        t->DIER = TIM_DIER_UIE;
        t->CR1 = TIM_CR1_URS | TIM_CR1_OPM | TIM_CR1_CEN;
    }
}

void arm_step_hw_init(void)
{
    uncertain = false;
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_TIM5_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF2_TIM5;
    HAL_GPIO_Init(GPIOA, &gpio);
    RCC_ClkInitTypeDef clock;
    uint32_t latency;
    HAL_RCC_GetClockConfig(&clock, &latency);
    timer_hz = HAL_RCC_GetPCLK1Freq() * (clock.APB1CLKDivider == RCC_HCLK_DIV1 ? 1U : 2U);
    channels[0] = (StepChannel){.timer=TIM2, .dir_port=ZL_DIR_GPIO_Port, .dir_pin=ZL_DIR_Pin, .dir_inverted=ARM_ZL_DIR_INVERTED};
    channels[1] = (StepChannel){.timer=TIM5, .dir_port=ZR_DIR_GPIO_Port, .dir_pin=ZR_DIR_Pin, .channel2=true, .dir_inverted=ARM_ZR_DIR_INVERTED};
    channels[2] = (StepChannel){.timer=TIM3, .dir_port=X_DIR_GPIO_Port, .dir_pin=X_DIR_Pin, .dir_inverted=ARM_X_DIR_INVERTED};
    channels[0].dma = DMA1_Stream1; channels[0].dma_channel = 3; channels[0].dma_shift = 6;
    channels[1].dma = DMA1_Stream0; channels[1].dma_channel = 6; channels[1].dma_shift = 0;
    channels[2].dma = DMA1_Stream2; channels[2].dma_channel = 5; channels[2].dma_shift = 16;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        TIM_TypeDef *t = channels[i].timer;
        t->CR1 = 0;
        t->CCER = 0;
        t->DIER = 0;
        t->SMCR = 0;
        t->CR2 = 0;
        /* PWM2: low setup/period, high tail, then overflow returns low and OPM stops. */
        t->CCMR1 = channels[i].channel2 ? (7U << TIM_CCMR1_OC2M_Pos) : (7U << TIM_CCMR1_OC1M_Pos);
        t->CNT = 0;
        t->SR = 0;
        t->DIER = TIM_DIER_UIE;
    }
    initialized = true;
    HAL_NVIC_SetPriority(TIM2_IRQn, 2, 0);
    HAL_NVIC_SetPriority(TIM5_IRQn, 2, 0);
    HAL_NVIC_SetPriority(TIM3_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    HAL_NVIC_EnableIRQ(TIM5_IRQn);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 2, 0);
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 2, 0);
    HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
}

bool arm_step_hw_apply(const ArmMotionOutputs *outputs)
{
    ArmStepPlan plans[ARM_AXIS_COUNT] = {0};
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        uint32_t frequency = outputs->axis[i].enabled ? outputs->axis[i].step_frequency_millihz : 0;
        if (frequency && !arm_step_plan(timer_hz, i == ARM_AXIS_X ? 16 : 32, frequency, &plans[i])) return false;
    }
    uint32_t irq = lock();
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        StepChannel *c = &channels[i];
        TIM_TypeDef *t = c->timer;
        uint32_t frequency = outputs->axis[i].enabled ? outputs->axis[i].step_frequency_millihz : 0;
        bool changed = frequency != c->requested_frequency || c->requested_positive != outputs->axis[i].dir_positive;
        c->requested_frequency = frequency;
        c->requested_positive = outputs->axis[i].dir_positive;
        c->plan = plans[i];
        account(c);
        if (changed && c->running && !c->dma_running) {
            /* Freeze before sampling CNT; never race the low-to-high edge. */
            t->CR1 &= ~TIM_CR1_CEN;
            account(c);
            if (c->running && t->CNT < compare(c)) {
                t->CCER = 0;
                c->running = false;
            } else if (c->running) {
                t->CR1 |= TIM_CR1_CEN;
            }
        }
        if (!c->running) start(c, (ArmAxisId)i);
    }
    __set_PRIMASK(irq);
    return true;
}

void arm_step_hw_irq(ArmAxisId axis)
{
    /* Serialize with the higher-priority watchdog and all configuration updates. */
    uint32_t irq = lock();
    StepChannel *c = &channels[axis];
    if (!c->dma_running && (c->timer->SR & TIM_SR_UIF)) {
        account(c);
        start(c, axis);
    }
    __set_PRIMASK(irq);
}

void arm_step_hw_dma_irq(ArmAxisId axis)
{
    uint32_t irq = lock();
    StepChannel *c = &channels[axis];
    uint32_t flags = DMA1->LISR >> c->dma_shift;
    /* Foreground may already have consumed the completed block and switched
     * to OPM (or stopped). Acknowledge stale flags as well: leaving TC set
     * keeps this IRQ pending forever and starves the foreground UART parser. */
    DMA1->LIFCR = (flags & 0x3DU) << c->dma_shift;
    if (c->dma_running && (flags & 0x0DU)) {
        arm_step_hw_abort();
        uncertain = true;
    } else if (c->dma_running && (flags & 0x20U)) {
        account(c);
        if (c->running) { arm_step_hw_abort(); uncertain = true; }
        else {
            DMA1->LIFCR = 0x3DU << c->dma_shift;
            c->dma_running = false;
            start(c, axis);
        }
    }
    __set_PRIMASK(irq);
}

void arm_step_hw_abort(void)
{
    if (!initialized) return;
    uint32_t irq = lock();
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        StepChannel *c = &channels[i];
        if (c->dma_running) stop_dma(c);
        c->timer->CR1 &= ~TIM_CR1_CEN;
        account(c);
        if (c->running && c->timer->CNT >= compare(c)) uncertain = true;
        c->timer->CCER = 0;
        c->requested_frequency = 0;
        c->running = false;
        c->limited = false;
        c->bidirectional_limit = false;
        c->remaining = 0;
    }
    __set_PRIMASK(irq);
}

void arm_step_hw_guard(void)
{
    if (!initialized) return;
    uint32_t irq = lock();
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        StepChannel *c = &channels[i];
        if (c->running && !arm_board_pulse_allowed((ArmAxisId)i, c->positive)) {
            if (c->dma_running) { stop_dma(c); c->requested_frequency = 0; continue; }
            c->timer->CR1 &= ~TIM_CR1_CEN;
            account(c);
            if (c->running && c->timer->CNT < compare(c)) {
                c->timer->CCER = 0;
                c->running = false;
            } else if (c->running) c->timer->CR1 |= TIM_CR1_CEN;
        }
    }
    __set_PRIMASK(irq);
}

bool arm_step_hw_begin_limit(ArmAxisId axis, uint32_t pulses, bool positive)
{
    if (!initialized || (unsigned)axis >= ARM_AXIS_COUNT || !pulses || pulses > 12800U) return false;
    uint32_t irq = lock();
    bool idle = true;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i)
        if (channels[i].running || channels[i].requested_frequency || channels[i].completed) idle = false;
    if (idle) {
        channels[axis].limited = true;
        channels[axis].bidirectional_limit = false;
        channels[axis].remaining = pulses;
        channels[axis].limit_positive = positive;
    }
    __set_PRIMASK(irq);
    return idle;
}

bool arm_step_hw_limit_done(ArmAxisId axis)
{
    if ((unsigned)axis >= ARM_AXIS_COUNT) return false;
    uint32_t irq = lock();
    bool done = channels[axis].limited && !channels[axis].remaining && !channels[axis].running;
    __set_PRIMASK(irq);
    return done;
}

bool arm_step_hw_begin_home_limit(void)
{
    if (!ARM_STANDALONE_ZL) return false;
    uint32_t irq = lock();
    bool ok = arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT, 1, false);
    if (ok) {
        /* Includes seek and all bounded switch-handling phases, both directions. */
        channels[0].remaining = (uint64_t)ARM_ZL_HOME_TOTAL_MAX_UM * ARM_DRIVER_PULSES_PER_REV / ARM_Z_UM_PER_REV;
        channels[0].bidirectional_limit = true;
    }
    __set_PRIMASK(irq);
    return ok;
}

void arm_step_hw_feedback(ArmMotionState *state)
{
    int64_t delta[ARM_AXIS_COUNT];
    bool active[ARM_AXIS_COUNT];
    uint32_t irq = lock();
    bool lost = uncertain;
    uncertain = false;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        if (channels[i].dma_running) account(&channels[i]);
        /* OPM UIF belongs to the IRQ that schedules the next pulse. */
        delta[i] = channels[i].completed;
        StepChannel *c = &channels[i];
        const ArmAxisState *a = &state->axis[i];
        c->position_bounded = a->homed && state->mode != ARM_MODE_HOMING;
        if (c->position_bounded) {
            int64_t scale = ARM_AXIS_CONFIG[i].driver_pulses_per_rev;
            int64_t lead = ARM_AXIS_CONFIG[i].um_per_motor_rev;
            int64_t position = a->position_numerator + delta[i] * lead;
            int64_t lower = (int64_t)a->soft_min_position_um * scale;
            int64_t upper = (int64_t)a->soft_max_position_um * scale;
            if (a->target_position_active) {
                int64_t target = (int64_t)a->target_position_um * scale;
                if (target >= position && target < upper) upper = target;
                if (target <= position && target > lower) lower = target;
            }
            c->positive_budget = (upper - position) / lead;
            c->negative_budget = (position - lower) / lead;
        }
        channels[i].completed = 0;
        active[i] = channels[i].running;
    }
    __set_PRIMASK(irq);
    for (int i = 0; i < ARM_AXIS_COUNT; ++i) {
        arm_motion_on_steps(state, (ArmAxisId)i, delta[i]);
        state->axis[i].pulse_active = active[i];
    }
    if (lost) arm_motion_latch_fault(state, ARM_FAULT_STEP_GENERATOR, ARM_AXIS_Z_LEFT);
}
