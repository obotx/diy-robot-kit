#include "stm32f4xx_hal.h"
#include "base_board.h"
#include <math.h>

static TIM_TypeDef *const enc[4] = {TIM1, TIM2, TIM3, TIM8};
static TIM_TypeDef *const motor[4] = {TIM4, TIM12, TIM12, TIM13};
static GPIO_TypeDef *const dir_port[4] = {GPIOC, GPIOC, GPIOB, GPIOB};
static const uint16_t dir_pin[4] = {GPIO_PIN_8, GPIO_PIN_9, GPIO_PIN_12, GPIO_PIN_13};
static uint16_t previous[4];
static int direction[4];
static bool initialized;
static volatile uint32_t *compare(int i) { return i == 2 ? &motor[i]->CCR2 : &motor[i]->CCR1; }
static uint32_t channel(int i) { return i == 2 ? TIM_CCER_CC2E : TIM_CCER_CC1E; }
static void gpio(GPIO_TypeDef *port, uint32_t pins, uint32_t af)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pins; g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_PULLDOWN;
    g.Speed = GPIO_SPEED_FREQ_LOW; g.Alternate = af;
    HAL_GPIO_Init(port, &g);
}
void base_board_off(void)
{
    if (!initialized) return;
    TIM4->CCER = 0; TIM12->CCER = 0; TIM13->CCER = 0;
    TIM4->CCR1 = TIM12->CCR1 = TIM12->CCR2 = TIM13->CCR1 = 0;
}
void base_board_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE(); __HAL_RCC_TIM2_CLK_ENABLE(); __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE(); __HAL_RCC_TIM4_CLK_ENABLE();
    __HAL_RCC_TIM12_CLK_ENABLE(); __HAL_RCC_TIM13_CLK_ENABLE();
    initialized = true; base_board_off();
    GPIOB->BSRR = (GPIO_PIN_12 | GPIO_PIN_13) << 16;
    GPIOC->BSRR = (GPIO_PIN_8 | GPIO_PIN_9) << 16;
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_PULLDOWN; g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = GPIO_PIN_12 | GPIO_PIN_13; HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9; HAL_GPIO_Init(GPIOC, &g);
    gpio(GPIOA, GPIO_PIN_8 | GPIO_PIN_9, 1);
    gpio(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, 1);
    gpio(GPIOB, GPIO_PIN_4 | GPIO_PIN_5, 2);
    gpio(GPIOC, GPIO_PIN_6 | GPIO_PIN_7, 3);
    for (int i = 0; i < 4; ++i) {
        enc[i]->CR1 = 0; enc[i]->PSC = 0; enc[i]->ARR = 65535;
        /* TI1/TI2 direct inputs, filter 6, quadrature on both edges (SMS=3). */
        enc[i]->CCMR1 = 1U | (6U << 4) | (1U << 8) | (6U << 12);
        enc[i]->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
        enc[i]->SMCR = 3; enc[i]->EGR = TIM_EGR_UG; enc[i]->CNT = 0;
        enc[i]->CR1 = TIM_CR1_CEN;
    }
    TIM_TypeDef *timers[] = {TIM4, TIM12, TIM13};
    for (int i = 0; i < 3; ++i) {
        timers[i]->CR1 = 0; timers[i]->PSC = 0; timers[i]->ARR = 799;
        /* HSI 16 MHz / 800 = 20 kHz, PWM mode 1, active high. */
        timers[i]->CCMR1 = (6U << 4) | (6U << 12);
        timers[i]->EGR = TIM_EGR_UG; timers[i]->CR1 = TIM_CR1_CEN;
    }
    gpio(GPIOB, GPIO_PIN_6, 2);
    gpio(GPIOB, GPIO_PIN_14 | GPIO_PIN_15, 9);
    gpio(GPIOA, GPIO_PIN_6, 9);
}
void base_board_counts(int32_t delta[4])
{
    for (int i = 0; i < 4; ++i) {
        uint16_t current = (uint16_t)enc[i]->CNT;
        uint16_t diff = (uint16_t)(current - previous[i]);
        delta[i] = diff >= 32768 ? (int32_t)diff - 65536 : diff;
        previous[i] = current;
    }
}
void base_board_apply(const float pwm[4])
{
    for (int i = 0; i < 4; ++i) {
        int sign = pwm[i] < 0 ? -1 : 1;
        if (!isfinite(pwm[i]) || fabsf(pwm[i]) > 1) { base_board_off(); return; }
        if (sign != direction[i]) {
            /* One full control interval with PWM off before changing direction. */
            motor[i]->CCER &= ~channel(i); *compare(i) = 0;
            dir_port[i]->BSRR = sign > 0 ? dir_pin[i] : (uint32_t)dir_pin[i] << 16;
            direction[i] = sign;
            continue;
        }
        *compare(i) = (uint32_t)(fabsf(pwm[i]) * 800.0f);
        motor[i]->CCER |= channel(i);
    }
}
