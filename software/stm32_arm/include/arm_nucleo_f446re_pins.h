#ifndef ARM_NUCLEO_F446RE_PINS_H
#define ARM_NUCLEO_F446RE_PINS_H

/*
 * Pin map for NUCLEO-F446RE / STM32F446RET6.
 *
 * Include this file from a CubeMX-generated project after stm32f4xx_hal.h is
 * available. Keep this file aligned with hardware/wiring/pinout-stm32-arm.md.
 */

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARM_ZL_STEP_GPIO_Port        GPIOA
#define ARM_ZL_STEP_Pin              GPIO_PIN_0
#define ARM_ZL_DIR_GPIO_Port         GPIOB
#define ARM_ZL_DIR_Pin               GPIO_PIN_0
#define ARM_ZL_ENABLE_GPIO_Port      GPIOB
#define ARM_ZL_ENABLE_Pin            GPIO_PIN_1
#define ARM_ZL_ALARM_GPIO_Port       GPIOC
#define ARM_ZL_ALARM_Pin             GPIO_PIN_0

#define ARM_ZR_STEP_GPIO_Port        GPIOA
#define ARM_ZR_STEP_Pin              GPIO_PIN_1
#define ARM_ZR_DIR_GPIO_Port         GPIOC
#define ARM_ZR_DIR_Pin               GPIO_PIN_1
#define ARM_ZR_ENABLE_GPIO_Port      GPIOC
#define ARM_ZR_ENABLE_Pin            GPIO_PIN_2
#define ARM_ZR_ALARM_GPIO_Port       GPIOC
#define ARM_ZR_ALARM_Pin             GPIO_PIN_3

#define ARM_X_STEP_GPIO_Port         GPIOA
#define ARM_X_STEP_Pin               GPIO_PIN_6
#define ARM_X_DIR_GPIO_Port          GPIOA
#define ARM_X_DIR_Pin                GPIO_PIN_7
#define ARM_X_ENABLE_GPIO_Port       GPIOB
#define ARM_X_ENABLE_Pin             GPIO_PIN_10
#define ARM_X_ALARM_GPIO_Port        GPIOC
#define ARM_X_ALARM_Pin              GPIO_PIN_4

#define ARM_ZL_MIN_GPIO_Port         GPIOC
#define ARM_ZL_MIN_Pin               GPIO_PIN_5
#define ARM_ZL_MAX_GPIO_Port         GPIOC
#define ARM_ZL_MAX_Pin               GPIO_PIN_6
#define ARM_ZR_MIN_GPIO_Port         GPIOC
#define ARM_ZR_MIN_Pin               GPIO_PIN_7
#define ARM_ZR_MAX_GPIO_Port         GPIOC
#define ARM_ZR_MAX_Pin               GPIO_PIN_8
#define ARM_X_MIN_GPIO_Port          GPIOC
#define ARM_X_MIN_Pin                GPIO_PIN_9
#define ARM_X_MAX_GPIO_Port          GPIOA
#define ARM_X_MAX_Pin                GPIO_PIN_8

#define ARM_SPARE_LIMIT_7_GPIO_Port  GPIOA
#define ARM_SPARE_LIMIT_7_Pin        GPIO_PIN_9
#define ARM_SPARE_LIMIT_8_GPIO_Port  GPIOA
#define ARM_SPARE_LIMIT_8_Pin        GPIO_PIN_10

#define ARM_ZL_BRAKE_GPIO_Port       GPIOB
#define ARM_ZL_BRAKE_Pin             GPIO_PIN_12
#define ARM_ZR_BRAKE_GPIO_Port       GPIOB
#define ARM_ZR_BRAKE_Pin             GPIO_PIN_13

#define ARM_GRIPPER_STEP_GPIO_Port   GPIOB
#define ARM_GRIPPER_STEP_Pin         GPIO_PIN_8
#define ARM_GRIPPER_DIR_GPIO_Port    GPIOB
#define ARM_GRIPPER_DIR_Pin          GPIO_PIN_7
#define ARM_GRIPPER_ENABLE_GPIO_Port GPIOB
#define ARM_GRIPPER_ENABLE_Pin       GPIO_PIN_9

#define ARM_MOTOR_DISABLE_GPIO_Port  GPIOC
#define ARM_MOTOR_DISABLE_Pin        GPIO_PIN_12

/*
 * Verify these polarity defaults against the installed electrical interfaces.
 * Confirm against the actual interface board and ZDM-2HA865 wiring.
 */
#define ARM_LIMIT_ACTIVE_STATE       GPIO_PIN_SET
#define ARM_ALARM_ACTIVE_STATE       GPIO_PIN_SET
#define ARM_MOTOR_POWER_OK_STATE     GPIO_PIN_SET
#define ARM_DRIVER_ENABLE_STATE      GPIO_PIN_SET
#define ARM_DRIVER_DISABLE_STATE     GPIO_PIN_RESET
#define ARM_BRAKE_RELEASE_STATE      GPIO_PIN_SET
#define ARM_BRAKE_ENGAGE_STATE       GPIO_PIN_RESET

#ifdef __cplusplus
}
#endif

#endif
