#include "main.h"
#include "arm_gripper.h"
#include <assert.h>
#include <stdio.h>
TIM_TypeDef mock_tim4;
GPIO_TypeDef mock_gpio;
static uint32_t now;
uint32_t HAL_GetTick(void) { return now; }
int main(void)
{
    arm_gripper_init();
    for (now=0; now<10000; now+=10) {
        arm_gripper_apply(1000,2000,10);
        assert(!arm_gripper_guard());
    }
    assert(TIM4->CR1 & TIM_CR1_CEN);
    assert(TIM4->PSC == 41);
    assert(TIM4->ARR == 124 && TIM4->CCR3 == 62);
    arm_gripper_apply(500,2000,10);
    assert(TIM4->ARR > 124);
    for (unsigned i=0; i<100; ++i) arm_gripper_apply(500,2000,10);
    assert(TIM4->ARR == 249);
    assert(mock_gpio.pins & GPIO_PIN_9);
    arm_gripper_apply(0,2000,10);
    assert(!(TIM4->CR1 & TIM_CR1_CEN));
    now+=999; arm_gripper_guard();
    assert(mock_gpio.pins & GPIO_PIN_9);
    now++; arm_gripper_guard();
    assert(!(mock_gpio.pins & GPIO_PIN_9));
    arm_gripper_apply(-1000,2000,10);
    now+=20; arm_gripper_apply(-1000,2000,10);
    assert(TIM4->CR1 & TIM_CR1_CEN);
    arm_gripper_stop();
    assert(!TIM4->CCER && !(mock_gpio.pins & GPIO_PIN_9));
    puts("PASS gripper: 16kHz, half speed, ramp, continuous 10s, release, idle hold, explicit stop");
}
