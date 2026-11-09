#include "main.h"
#include "arm_gripper.h"

/* Trial ceiling: 300 rpm at 200 full steps/rev and 1/16 microstep. */
#define GRIPPER_MAX_STEP_HZ 16000
#define GRIPPER_TIMER_HZ 2000000U
static bool initialized, positive;
static volatile bool active;
static volatile bool idle_hold;
static volatile uint32_t idle_started;
static uint32_t settled;
static volatile int32_t speed;
static volatile int64_t ramp_remainder;

void arm_gripper_stop(void)
{
    if (!initialized) return;
    TIM4->CR1 = 0;
    TIM4->CCER = 0;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    active = false;
    idle_hold = false;
    speed = 0;
    ramp_remainder = 0;
}

void arm_gripper_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7 | GPIO_PIN_9, GPIO_PIN_RESET);
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_7 | GPIO_PIN_9;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_PULLDOWN;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_8;
    g.Mode = GPIO_MODE_AF_PP;
    g.Alternate = GPIO_AF2_TIM4;
    HAL_GPIO_Init(GPIOB, &g);
    RCC_ClkInitTypeDef clocks;
    uint32_t latency;
    HAL_RCC_GetClockConfig(&clocks, &latency);
    uint32_t hz = HAL_RCC_GetPCLK1Freq() * (clocks.APB1CLKDivider == RCC_HCLK_DIV1 ? 1U : 2U);
    TIM4->CR1 = TIM4->DIER = TIM4->CCER = 0;
    TIM4->PSC = hz / GRIPPER_TIMER_HZ - 1U;
    TIM4->CCMR2 = (6U << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
    TIM4->ARR = 999;
    TIM4->CCR3 = 0;
    TIM4->EGR = TIM_EGR_UG;
    initialized = true;
    arm_gripper_stop();
}

/* Called from SysTick, independently of UART and main-loop progress. */
bool arm_gripper_guard(void)
{
    if (idle_hold && (uint32_t)(HAL_GetTick() - idle_started) >= 1000U)
        arm_gripper_stop();
    return false;
}

void arm_gripper_apply(int32_t target, int32_t acceleration, uint32_t dt)
{
    if (!initialized) return;
    if (!target) {
        if (active) {
            TIM4->CR1 = 0;
            TIM4->CCER = 0;
            active = false;
            speed = 0;
            ramp_remainder = 0;
            idle_started = HAL_GetTick();
            idle_hold = true;
        }
        return;
    }
    bool direction = target > 0;
    int32_t requested = direction ? target : -target;
    if (requested > 1000) requested = 1000;
    requested = requested * GRIPPER_MAX_STEP_HZ / 1000;
    if (!active) {
        idle_hold = false;
        settled = HAL_GetTick();
        positive = direction;
        active = true;
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, direction ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    }
    if (direction != positive) {
        TIM4->CR1 = TIM4->CCER = 0;
        speed = 0;
        ramp_remainder = 0;
        positive = direction;
        settled = HAL_GetTick();
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, direction ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    if ((uint32_t)(HAL_GetTick() - settled) < 10U) return;
    if (dt > 20U) dt = 20U;
    ramp_remainder += (int64_t)acceleration * dt * (GRIPPER_MAX_STEP_HZ / 1000);
    int32_t change = (int32_t)(ramp_remainder / 1000);
    ramp_remainder %= 1000;
    if (speed < requested) speed = speed + change > requested ? requested : speed + change;
    else if (speed > requested) speed = speed - change < requested ? requested : speed - change;
    if (!speed) return;
    uint32_t period = GRIPPER_TIMER_HZ / (uint32_t)speed;
    if (period > 65536U) period = 65536U;
    TIM4->ARR = period - 1U;
    TIM4->CCR3 = period / 2U;
    if (!(TIM4->CR1 & TIM_CR1_CEN)) {
        TIM4->CNT = 0;
        TIM4->EGR = TIM_EGR_UG;
        TIM4->CCER = TIM_CCER_CC3E;
        TIM4->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    }
}
