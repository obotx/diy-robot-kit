/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "arm_debug_protocol.h"
#include "arm_link.h"
#include "arm_board_link.h"
#include "arm_step_hw.h"
#include "arm_gripper.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RX_LINE_BUFFER_SIZE 80U
#define TX_STATUS_BUFFER_SIZE 512U
#define CONTROL_TICK_MS 10U
#define UART_RX_RING_SIZE 4096U

#define LIMIT_ACTIVE_STATE GPIO_PIN_SET
#define ZL_MIN_ACTIVE_STATE GPIO_PIN_RESET
#define ZL_MAX_ACTIVE_STATE GPIO_PIN_RESET
#include "arm_step_burst.h"
#include "arm_uart_tx.h"
#define ALARM_ACTIVE_STATE GPIO_PIN_SET
#define DRIVER_ENABLE_STATE GPIO_PIN_SET
#define DRIVER_DISABLE_STATE GPIO_PIN_RESET

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static ArmMotionState arm_state;
static ArmMotionOutputs arm_outputs;
static uint8_t rx_byte;
static char rx_line[RX_LINE_BUFFER_SIZE];
static uint32_t rx_line_len;
static uint32_t last_control_tick_ms;
static ArmLink arm_link;
static volatile bool gripper_expired;
static char link_reply[ARM_LINK_TX_SIZE];
static ArmUartTx link_tx;
static uint8_t uart_rx_ring[UART_RX_RING_SIZE];
static volatile uint32_t uart_rx_head, uart_rx_tail;
static volatile bool uart_rx_error;
static volatile bool board_started;
static volatile bool service_armed, service_expired;
static volatile uint32_t service_last_ms;
static bool service_dropping;
static volatile ArmFaultCode input_fault;
static bool drivers_enabled;
static uint32_t drivers_enabled_ms;
static bool bench_travel_running;
static volatile bool bench_home_running;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
static void arm_read_inputs(ArmMotionInputs *inputs);
static void arm_apply_outputs(const ArmMotionOutputs *outputs);
static void arm_poll_uart(void);
static void arm_send_text(const char *text);
static void arm_send_status(void);
static void arm_handle_line(const char *line);
static void arm_update_feedback(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  arm_motion_init(&arm_state);
  arm_step_hw_init();
  arm_gripper_init();
  if (ARM_STEP_SCOPE_DMA) arm_step_burst_init();
  arm_motion_command_stop(&arm_state);
  arm_apply_outputs(&arm_outputs);
  if (ARM_SERIAL_SERVICE_MODE) {
    arm_send_text("OBOTX ARM SERVICE MODE\r\n");
  } else {
    char device_id[81], boot_id[81];
    if (!arm_board_identity(device_id, boot_id)) Error_Handler();
    arm_link_init(&arm_link, device_id, boot_id, ARM_WEB_MOTION_COMMISSIONED != 0);
  }
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
  HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  if (HAL_UART_Receive_IT(&huart2, &rx_byte, 1U) != HAL_OK) Error_Handler();
  board_started = true;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now_ms = HAL_GetTick();

    if (ARM_STEP_SCOPE_DMA) {
      arm_poll_uart();
      arm_step_burst_poll();
      continue;
    }

    arm_update_feedback();
    if (input_fault != ARM_FAULT_NONE) {
      arm_motion_latch_fault(&arm_state, input_fault, ARM_AXIS_Z_LEFT);
      input_fault = ARM_FAULT_NONE;
    }
    if (service_expired) {
      arm_motion_latch_fault(&arm_state, ARM_FAULT_COMM_TIMEOUT, ARM_AXIS_Z_LEFT);
      service_expired = false;
    }

    arm_poll_uart();
    if (gripper_expired) {
      arm_link_abort(&arm_link, &arm_state, "gripper_timeout");
      arm_board_emergency_stop();
      gripper_expired = false;
    }
    now_ms = HAL_GetTick();

    if (!ARM_SERIAL_SERVICE_MODE) {
      ArmMotionInputs inputs;
      arm_read_inputs(&inputs);
      arm_link_tick(&arm_link, &arm_state, &inputs, now_ms);
      if (!arm_state.enabled) arm_board_emergency_stop();
    }

    if ((uint32_t)(now_ms - last_control_tick_ms) >= CONTROL_TICK_MS) {
      ArmMotionInputs inputs;

      arm_update_feedback();
      uint32_t elapsed_ms = now_ms - last_control_tick_ms;
      last_control_tick_ms = now_ms;
      arm_read_inputs(&inputs);
      arm_motion_tick(&arm_state, &inputs, elapsed_ms, &arm_outputs);
      if (!ARM_SERIAL_SERVICE_MODE) arm_link_tick(&arm_link, &arm_state, &inputs, now_ms);
      arm_apply_outputs(&arm_outputs);
      uint32_t gripper_irq = __get_PRIMASK();
      __disable_irq();
      if (!ARM_SERIAL_SERVICE_MODE && arm_state.enabled && !arm_state.faulted &&
          !arm_link.watchdog_expired && !gripper_expired && !uart_rx_error &&
          input_fault == ARM_FAULT_NONE && inputs.motor_power_enabled && inputs.e_stop_ok)
        arm_gripper_apply(arm_link.gripper_target_milli, arm_link.gripper_accel_milli, elapsed_ms);
      else arm_gripper_stop();
      __set_PRIMASK(gripper_irq);
    }

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 65535;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, ZR_DIR_Pin|ZR_ENABLE_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LD2_Pin|X_DIR_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, ZL_DIR_Pin|ZL_ENABLE_Pin|X_ENABLE_Pin|ZL_BRAKE_RELEASE_Pin
                          |ZR_BRAKE_RELEASE_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : ZL_ALARM_Pin ZR_ALARM_Pin X_ALARM_Pin ZL_MIN_Pin
                           ZL_MAX_Pin ZR_MIN_Pin ZR_MAX_Pin X_MIN_Pin */
  GPIO_InitStruct.Pin = ZL_ALARM_Pin|ZR_ALARM_Pin|X_ALARM_Pin|ZL_MIN_Pin
                          |ZL_MAX_Pin|ZR_MIN_Pin|ZR_MAX_Pin|X_MIN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : ZR_DIR_Pin ZR_ENABLE_Pin */
  GPIO_InitStruct.Pin = ZR_DIR_Pin|ZR_ENABLE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : LD2_Pin X_DIR_Pin */
  GPIO_InitStruct.Pin = LD2_Pin|X_DIR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : ZL_DIR_Pin ZL_ENABLE_Pin X_ENABLE_Pin ZL_BRAKE_RELEASE_Pin
                           ZR_BRAKE_RELEASE_Pin */
  GPIO_InitStruct.Pin = ZL_DIR_Pin|ZL_ENABLE_Pin|X_ENABLE_Pin|ZL_BRAKE_RELEASE_Pin
                          |ZR_BRAKE_RELEASE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : X_MAX_Pin */
  GPIO_InitStruct.Pin = X_MAX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(X_MAX_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* Home sensors PC5/PC7/PC9 and level PA8: identical active-low PC817 outputs. */
  GPIO_InitStruct.Pin = ZL_MIN_Pin | ZR_MIN_Pin | X_MIN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(ZL_MIN_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = X_MAX_Pin;
  HAL_GPIO_Init(X_MAX_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static bool gpio_is_active(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState active_state)
{
  return HAL_GPIO_ReadPin(port, pin) == active_state;
}

static void arm_read_inputs(ArmMotionInputs *inputs)
{
  memset(inputs, 0, sizeof(*inputs));

  inputs->axis[ARM_AXIS_Z_LEFT].driver_alarm_active =
      gpio_is_active(ZL_ALARM_GPIO_Port, ZL_ALARM_Pin, ALARM_ACTIVE_STATE);
  inputs->axis[ARM_AXIS_Z_RIGHT].driver_alarm_active =
      gpio_is_active(ZR_ALARM_GPIO_Port, ZR_ALARM_Pin, ALARM_ACTIVE_STATE);
  inputs->axis[ARM_AXIS_X].driver_alarm_active =
      gpio_is_active(X_ALARM_GPIO_Port, X_ALARM_Pin, ALARM_ACTIVE_STATE);

  inputs->axis[ARM_AXIS_Z_LEFT].min_limit_active =
      gpio_is_active(ZL_MIN_GPIO_Port, ZL_MIN_Pin, ZL_MIN_ACTIVE_STATE);
  inputs->axis[ARM_AXIS_Z_LEFT].max_limit_active =
      false;
  inputs->axis[ARM_AXIS_Z_RIGHT].min_limit_active =
      gpio_is_active(ZR_MIN_GPIO_Port, ZR_MIN_Pin, GPIO_PIN_RESET);
  inputs->axis[ARM_AXIS_Z_RIGHT].max_limit_active =
      false;
  inputs->axis[ARM_AXIS_X].min_limit_active =
      gpio_is_active(X_MIN_GPIO_Port, X_MIN_Pin, GPIO_PIN_RESET);
  inputs->axis[ARM_AXIS_X].max_limit_active =
      false;
  inputs->level_active = gpio_is_active(X_MAX_GPIO_Port, X_MAX_Pin, GPIO_PIN_RESET);

  /* No power feedback is wired; this is an assumption, not a voltage measurement. */
  inputs->motor_power_enabled = true;
  inputs->e_stop_ok = true;
}

static void arm_apply_outputs(const ArmMotionOutputs *outputs)
{
  uint32_t irq = __get_PRIMASK();
  __disable_irq();
  if (!(ARM_SERIAL_SERVICE_MODE ? ARM_BENCH_OUTPUTS : ARM_WEB_MOTION_COMMISSIONED) ||
      !arm_state.enabled || (!ARM_SERIAL_SERVICE_MODE && arm_link.watchdog_expired) ||
      service_expired || input_fault != ARM_FAULT_NONE || uart_rx_error || arm_state.faulted) {
    arm_board_emergency_stop();
    __set_PRIMASK(irq);
    return;
  }
  HAL_GPIO_WritePin(
      ZL_ENABLE_GPIO_Port,
      ZL_ENABLE_Pin,
      outputs->axis[ARM_AXIS_Z_LEFT].enabled ? DRIVER_ENABLE_STATE : DRIVER_DISABLE_STATE);
  HAL_GPIO_WritePin(
      ZR_ENABLE_GPIO_Port,
      ZR_ENABLE_Pin,
      !ARM_STANDALONE_ZL && !ARM_JSON_ZL_ONLY && outputs->axis[ARM_AXIS_Z_RIGHT].enabled ? DRIVER_ENABLE_STATE : DRIVER_DISABLE_STATE);
  HAL_GPIO_WritePin(
      X_ENABLE_GPIO_Port,
      X_ENABLE_Pin,
      !ARM_STANDALONE_ZL && !ARM_JSON_ZL_ONLY && outputs->axis[ARM_AXIS_X].enabled ? DRIVER_ENABLE_STATE : DRIVER_DISABLE_STATE);

  if (!drivers_enabled) {
    drivers_enabled = true;
    drivers_enabled_ms = HAL_GetTick();
  }
  /* Explicit ENA setup before changing DIR or starting the first pulse. */
  if ((uint32_t)(HAL_GetTick() - drivers_enabled_ms) < ARM_DRIVER_ENABLE_SETUP_MS) {
    __set_PRIMASK(irq);
    return;
  }

  if (!arm_step_hw_apply(outputs)) {
    arm_motion_latch_fault(&arm_state, ARM_FAULT_STEP_GENERATOR, ARM_AXIS_Z_LEFT);
    arm_board_emergency_stop();
  }

  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, outputs->faulted ? GPIO_PIN_SET : GPIO_PIN_RESET);
  __set_PRIMASK(irq);
}

void arm_board_emergency_stop(void)
{
  arm_gripper_stop();
  if (ARM_STEP_SCOPE_DMA) arm_step_burst_abort();
  drivers_enabled = false;
  arm_step_hw_abort();
  /* Also handles failures before the pulse engine has initialized. */
  TIM2->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC2E);
  TIM3->CCER &= ~TIM_CCER_CC1E;
  TIM2->CR1 &= ~TIM_CR1_CEN;
  TIM3->CR1 &= ~TIM_CR1_CEN;
  HAL_GPIO_WritePin(ZL_ENABLE_GPIO_Port, ZL_ENABLE_Pin, DRIVER_DISABLE_STATE);
  HAL_GPIO_WritePin(ZR_ENABLE_GPIO_Port, ZR_ENABLE_Pin, DRIVER_DISABLE_STATE);
  HAL_GPIO_WritePin(X_ENABLE_GPIO_Port, X_ENABLE_Pin, DRIVER_DISABLE_STATE);
  HAL_GPIO_WritePin(ZL_BRAKE_RELEASE_GPIO_Port, ZL_BRAKE_RELEASE_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(ZR_BRAKE_RELEASE_GPIO_Port, ZR_BRAKE_RELEASE_Pin, GPIO_PIN_RESET);
}

void arm_board_watchdog_tick(void)
{
  if (ARM_STEP_SCOPE_DMA) {
    if (board_started) arm_step_burst_poll();
    return;
  }
  if (board_started) {
    if (arm_gripper_guard()) {
      gripper_expired = true;
      arm_board_emergency_stop();
    }
    ArmMotionInputs inputs;
    arm_read_inputs(&inputs);
    if (!inputs.motor_power_enabled) input_fault = ARM_FAULT_MOTOR_POWER_DISABLED;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i)
      if (inputs.axis[i].driver_alarm_active) input_fault = ARM_FAULT_DRIVER_ALARM;
    if (input_fault != ARM_FAULT_NONE) arm_board_emergency_stop();
    arm_step_hw_guard();
  }
  if (board_started && ARM_SERIAL_SERVICE_MODE && service_armed &&
      (uint32_t)(HAL_GetTick() - service_last_ms) >= 1000U) {
    service_expired = true;
    service_armed = false;
    arm_board_emergency_stop();
  }
  if (board_started && !ARM_SERIAL_SERVICE_MODE &&
      arm_link_watchdog_isr(&arm_link, HAL_GetTick())) arm_board_emergency_stop();
}

bool arm_board_pulse_allowed(ArmAxisId axis, bool positive)
{
  if ((ARM_STANDALONE_ZL || ARM_JSON_ZL_ONLY) && axis != ARM_AXIS_Z_LEFT) return false;
  if (service_expired || input_fault != ARM_FAULT_NONE || uart_rx_error || (!ARM_SERIAL_SERVICE_MODE && arm_link.watchdog_expired)) return false;
  ArmMotionInputs inputs;
  arm_read_inputs(&inputs);
  if (!inputs.motor_power_enabled || !inputs.e_stop_ok) return false;
  if (bench_home_running && inputs.axis[ARM_AXIS_Z_LEFT].max_limit_active) return false;
  for (int i = 0; i < ARM_AXIS_COUNT; ++i) if (inputs.axis[i].driver_alarm_active) return false;
  if (arm_link.service_release_required) return false;
  if (arm_state.service_bypass && arm_link.service_active) return true;
  return !(positive ? inputs.axis[axis].max_limit_active : inputs.axis[axis].min_limit_active);
}

static void arm_poll_uart(void)
{
  if (uart_rx_error) {
    uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    uart_rx_tail = uart_rx_head;
    uart_rx_error = false;
    __set_PRIMASK(interrupt_state);
    arm_link_abort(&arm_link, &arm_state, "uart_error_or_overflow");
    arm_link.rx_len = 0;
    arm_link.dropping = true;
    rx_line_len = 0;
    service_dropping = true;
    arm_board_emergency_stop();
  }
  if (!ARM_SERIAL_SERVICE_MODE) {
    ArmTxStatus tx = arm_uart_tx_poll(&link_tx, &huart2, HAL_GetTick());
    if (tx == ARM_TX_FAILED) {
      arm_link_abort(&arm_link, &arm_state, "uart_tx_error");
      arm_board_emergency_stop();
      return;
    }
    /* Do not overwrite link_reply while the TX interrupt is reading it.
     * Return to motion control instead of blocking on the wire transfer. */
    if (tx == ARM_TX_BUSY) return;
  }
  for (unsigned budget = 0; budget < 128U && uart_rx_tail != uart_rx_head; ++budget) {
    uint8_t byte = uart_rx_ring[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1U) % UART_RX_RING_SIZE;
    if (!ARM_SERIAL_SERVICE_MODE) {
      ArmMotionInputs inputs;
      arm_read_inputs(&inputs);
      size_t length = arm_link_feed(&arm_link, &arm_state, &inputs, HAL_GetTick(),
                                    byte, link_reply, sizeof(link_reply));
      if (!arm_state.enabled) arm_board_emergency_stop();
      if (length) {
        arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
        arm_apply_outputs(&arm_outputs);
        if (!arm_uart_tx_start(&link_tx, &huart2, (uint8_t *)link_reply, (uint16_t)length, HAL_GetTick())) {
          arm_link_abort(&arm_link, &arm_state, "uart_tx_error");
          arm_board_emergency_stop();
        }
        return;
      }
      continue;
    }
    if (service_dropping) {
      if (byte == '\n') service_dropping = false;
      continue;
    }
    if (byte == '\r' || byte == '\n') {
      if (rx_line_len > 0U) {
        rx_line[rx_line_len] = '\0';
        arm_handle_line(rx_line);
        rx_line_len = 0U;
      }
      continue;
    }

    if (rx_line_len < (RX_LINE_BUFFER_SIZE - 1U)) {
      rx_line[rx_line_len++] = (char)byte;
    } else {
      rx_line_len = 0U;
      service_dropping = true;
      arm_motion_set_enabled(&arm_state, false);
      arm_board_emergency_stop();
      arm_send_text("ERR line_too_long\r\n");
    }
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
  if (uart->Instance != USART2) return;
  uint32_t next = (uart_rx_head + 1U) % UART_RX_RING_SIZE;
  if (next == uart_rx_tail) {
    uart_rx_error = true;
    arm_board_emergency_stop();
  } else {
    uart_rx_ring[uart_rx_head] = rx_byte;
    __DMB();
    uart_rx_head = next;
  }
  if (HAL_UART_Receive_IT(uart, &rx_byte, 1U) != HAL_OK) uart_rx_error = true;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  if (uart->Instance != USART2) return;
  uart_rx_error = true;
  arm_board_emergency_stop();
  HAL_UART_AbortReceive(uart);
  __HAL_UART_CLEAR_OREFLAG(uart);
  (void)HAL_UART_Receive_IT(uart, &rx_byte, 1U);
}

static void arm_send_text(const char *text)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)text, (uint16_t)strlen(text), 100U);
}

static void arm_send_status(void)
{
  char tx_status[TX_STATUS_BUFFER_SIZE];
  size_t len = arm_debug_format_status(&arm_state, &arm_outputs, tx_status, sizeof(tx_status));

  HAL_UART_Transmit(&huart2, (uint8_t *)tx_status, (uint16_t)len, 100U);
}

static void arm_update_feedback(void)
{
  arm_step_hw_feedback(&arm_state);
  if (bench_home_running && (!arm_state.enabled || arm_state.mode != ARM_MODE_HOMING ||
      arm_step_hw_limit_done(ARM_AXIS_Z_LEFT))) {
    if (arm_state.enabled && arm_state.mode == ARM_MODE_HOMING)
      arm_motion_latch_fault(&arm_state, ARM_FAULT_HOME_SWITCH, ARM_AXIS_Z_LEFT);
    arm_motion_set_enabled(&arm_state, false);
    service_armed = false;
    arm_board_emergency_stop();
    arm_step_hw_feedback(&arm_state);
    bench_home_running = false;
    ArmMotionInputs inputs;
    arm_read_inputs(&inputs);
    arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
  }
  if (bench_travel_running && (!arm_state.enabled || arm_step_hw_limit_done(ARM_AXIS_Z_LEFT))) {
    ArmMotionInputs inputs;
    arm_motion_set_enabled(&arm_state, false);
    service_armed = false;
    arm_board_emergency_stop();
    /* Include the last ISR batch before reporting completion. */
    arm_step_hw_feedback(&arm_state);
    bench_travel_running = false;
    arm_read_inputs(&inputs);
    arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
  }
}

static bool arm_start_bench_travel(const ArmDebugCommand *command, const ArmMotionInputs *inputs)
{
  int64_t distance = command->position_um;
  int64_t magnitude = distance < 0 ? -distance : distance;
  int32_t velocity = command->velocity_limit_um_s ? command->velocity_limit_um_s : 1000;
  int64_t skew = (int64_t)arm_state.axis[0].position_um + distance - arm_state.axis[1].position_um;
  if (!ARM_STANDALONE_ZL || !command->physical_axis_selected || command->physical_axis != ARM_AXIS_Z_LEFT ||
      arm_state.enabled || arm_state.faulted || arm_state.mode != ARM_MODE_IDLE || bench_travel_running ||
      !inputs->motor_power_enabled || !inputs->e_stop_ok || magnitude == 0 || magnitude > 20000 ||
      velocity < 1 || velocity > ARM_Z_STEP_MAX_VELOCITY_UM_S || skew < -ARM_Z_MAX_SKEW_UM || skew > ARM_Z_MAX_SKEW_UM ||
      magnitude * ARM_DRIVER_PULSES_PER_REV % ARM_Z_UM_PER_REV != 0) return false;
  /* Faster bench moves go away from MIN only, inside the first 80 mm after home. */
  if (velocity > 1000 && (!arm_state.axis[0].homed || distance <= 0 ||
      arm_state.axis[0].position_um < 0 || (int64_t)arm_state.axis[0].position_um + distance > 80000)) return false;
  for (int i = 0; i < ARM_AXIS_COUNT; ++i)
    if (inputs->axis[i].min_limit_active || inputs->axis[i].max_limit_active ||
        inputs->axis[i].driver_alarm_active || arm_state.axis[i].pulse_active) return false;
  uint32_t irq = __get_PRIMASK();
  __disable_irq();
  bool ok = !service_expired && input_fault == ARM_FAULT_NONE && !uart_rx_error;
  if (ok) ok = arm_step_hw_begin_limit(ARM_AXIS_Z_LEFT,
      (uint32_t)(magnitude * ARM_DRIVER_PULSES_PER_REV / ARM_Z_UM_PER_REV), distance > 0);
  if (ok) {
    arm_motion_set_enabled(&arm_state, true);
    ok = arm_motion_command_axis_jog(&arm_state, ARM_AXIS_Z_LEFT, distance > 0 ? velocity : -velocity);
  }
  if (ok) {
    bench_travel_running = true;
    service_last_ms = HAL_GetTick();
    service_armed = true;
  } else {
    arm_motion_set_enabled(&arm_state, false);
    arm_board_emergency_stop();
  }
  __set_PRIMASK(irq);
  return ok;
}

static bool scope_number(const char *text, uint32_t maximum, uint32_t *value)
{
  uint32_t n = 0;
  if (!text || !*text) return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    uint32_t digit = (uint32_t)(*text - '0');
    if (n > maximum / 10U || n * 10U + digit > maximum) return false;
    n = n * 10U + digit;
  }
  *value = n;
  return n != 0;
}

static void arm_handle_scope_line(const char *line)
{
  char copy[80], reply[160];
  if (strlen(line) >= sizeof(copy)) { arm_send_text("ERR scope_command\r\n"); return; }
  strcpy(copy, line);
  char *tokens[5];
  unsigned count = 0;
  for (char *p = strtok(copy, " \t"); p; p = strtok(NULL, " \t")) {
    if (count == 5) { arm_send_text("ERR scope_command\r\n"); return; }
    tokens[count++] = p;
  }
  if (count == 1 && !strcmp(tokens[0], "STOP")) arm_step_burst_abort();
  else if (count == 2 && !strcmp(tokens[0], "SCOPE") && !strcmp(tokens[1], "STATUS")) {}
  else if (count == 4 && !strcmp(tokens[0], "SCOPE")) {
    ArmAxisId axis = !strcmp(tokens[1], "ZL") ? ARM_AXIS_Z_LEFT :
                     (!strcmp(tokens[1], "ZR") ? ARM_AXIS_Z_RIGHT : ARM_AXIS_X);
    uint32_t hz, pulses;
    if ((strcmp(tokens[1], "ZL") && strcmp(tokens[1], "ZR") && strcmp(tokens[1], "X")) ||
        !scope_number(tokens[2], ARM_BURST_MAX_HZ, &hz) ||
        !scope_number(tokens[3], ARM_BURST_MAX_PULSES, &pulses) ||
        !arm_step_burst_start(axis, hz, pulses)) {
      arm_send_text("ERR scope_busy_or_range\r\n"); return;
    }
  } else { arm_send_text("ERR scope_only\r\n"); return; }
  ArmBurstStatus s = arm_step_burst_status();
  snprintf(reply, sizeof(reply), "SCOPE state=%u axis=%u hz=%lu pulses=%lu ena=0\r\n",
           (unsigned)s.state, (unsigned)s.axis, (unsigned long)s.hz, (unsigned long)s.pulses);
  arm_send_text(reply);
}

static void arm_handle_line(const char *line)
{
  if (ARM_STEP_SCOPE_DMA) { arm_handle_scope_line(line); return; }
  ArmDebugCommand command;
  ArmMotionInputs inputs;
  arm_update_feedback();
  arm_read_inputs(&inputs);
  if (!strcmp(line, "INPUTS") || !strcmp(line, "IO")) {
    char text[240];
    snprintf(text, sizeof(text), "IO power=%u zl=%u,%u,%u zr=%u,%u,%u x=%u,%u,%u filtered=%u,%u,%u,%u,%u,%u bench=%u standalone_zl=%u skew_um=%ld\r\n",
        inputs.motor_power_enabled,
        inputs.axis[0].min_limit_active, inputs.axis[0].max_limit_active, inputs.axis[0].driver_alarm_active,
        inputs.axis[1].min_limit_active, inputs.axis[1].max_limit_active, inputs.axis[1].driver_alarm_active,
        inputs.axis[2].min_limit_active, inputs.axis[2].max_limit_active, inputs.axis[2].driver_alarm_active,
        arm_state.axis[0].min_filter.stable, arm_state.axis[0].max_filter.stable,
        arm_state.axis[1].min_filter.stable, arm_state.axis[1].max_filter.stable,
        arm_state.axis[2].min_filter.stable, arm_state.axis[2].max_filter.stable, ARM_BENCH_OUTPUTS,
        ARM_STANDALONE_ZL, (long)ARM_Z_MAX_SKEW_UM);
    arm_send_text(text);
    return;
  }

  if (!arm_debug_parse_line(line, &command)) {
    arm_send_text("ERR parse\r\n");
    return;
  }

  if (ARM_STANDALONE_ZL && command.type == ARM_DEBUG_CMD_HOME && command.physical_axis_selected) {
    bool ok = command.physical_axis == ARM_AXIS_Z_LEFT && !arm_state.enabled && !arm_state.faulted &&
        arm_state.mode == ARM_MODE_IDLE && !bench_travel_running && !bench_home_running &&
        inputs.motor_power_enabled && inputs.e_stop_ok;
    for (int i = 0; i < ARM_AXIS_COUNT; ++i)
      if (inputs.axis[i].driver_alarm_active || inputs.axis[i].max_limit_active ||
          inputs.axis[i].min_limit_active || arm_state.axis[i].pulse_active) ok = false;
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    ok = ok && !service_expired && input_fault == ARM_FAULT_NONE && !uart_rx_error;
    if (ok) ok = arm_step_hw_begin_home_limit();
    if (ok) {
      arm_motion_set_enabled(&arm_state, true);
      ok = arm_motion_command_home_zl(&arm_state);
      if (ok) {
        bench_home_running = true;
        service_last_ms = HAL_GetTick();
        service_armed = true;
      } else {
        arm_motion_set_enabled(&arm_state, false);
        arm_board_emergency_stop();
      }
    }
    __set_PRIMASK(irq);
    if (!ok) { arm_send_text("ERR standalone_home_precondition\r\n"); return; }
    arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
    arm_apply_outputs(&arm_outputs);
    arm_send_text("OK\r\n");
    arm_send_status();
    return;
  }

  if (command.type == ARM_DEBUG_CMD_TRAVEL) {
    if (!arm_start_bench_travel(&command, &inputs)) {
      arm_send_text("ERR standalone_travel_precondition\r\n");
      return;
    }
    arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
    arm_apply_outputs(&arm_outputs);
    arm_send_text("OK\r\n");
    arm_send_status();
    return;
  }
  if (ARM_STANDALONE_ZL && command.type != ARM_DEBUG_CMD_HEARTBEAT &&
      command.type != ARM_DEBUG_CMD_STATUS && command.type != ARM_DEBUG_CMD_ENABLE &&
      command.type != ARM_DEBUG_CMD_STOP && command.type != ARM_DEBUG_CMD_CLEAR_FAULT) {
    arm_send_text("ERR standalone_use_TRAVEL\r\n");
    return;
  }
  if (ARM_STANDALONE_ZL && command.type == ARM_DEBUG_CMD_STOP) {
    arm_motion_set_enabled(&arm_state, false);
    arm_board_emergency_stop();
    arm_update_feedback();
  }

  if (command.type == ARM_DEBUG_CMD_JOG || command.type == ARM_DEBUG_CMD_MOVE ||
      command.type == ARM_DEBUG_CMD_SET_MOVE_VELOCITY) {
    ArmAxisId axis = command.physical_axis_selected ? command.physical_axis :
        (command.axis == ARM_VIRTUAL_AXIS_X ? ARM_AXIS_X : ARM_AXIS_Z_LEFT);
    int64_t velocity = command.type == ARM_DEBUG_CMD_MOVE ? command.velocity_limit_um_s : command.velocity_um_s;
    if (command.type == ARM_DEBUG_CMD_MOVE && velocity == 0) velocity = arm_state.axis[axis].default_move_velocity_um_s;
    if (velocity < 0) velocity = -velocity;
    if (velocity * ARM_AXIS_CONFIG[axis].driver_pulses_per_rev >
        (int64_t)ARM_STEP_MAX_HZ * ARM_AXIS_CONFIG[axis].um_per_motor_rev) {
      arm_send_text("ERR bench_step_rate\r\n");
      return;
    }
  }

  if (!arm_debug_apply_command(&arm_state, &command)) {
    arm_send_text("ERR command\r\n");
    return;
  }

  if (command.type == ARM_DEBUG_CMD_HEARTBEAT || command.type == ARM_DEBUG_CMD_ENABLE ||
      command.type == ARM_DEBUG_CMD_JOG || command.type == ARM_DEBUG_CMD_MOVE || command.type == ARM_DEBUG_CMD_HOME) {
    service_last_ms = HAL_GetTick();
    service_armed = arm_state.enabled;
  }
  if (!arm_state.enabled) service_armed = false;
  arm_motion_tick(&arm_state, &inputs, 0, &arm_outputs);
  arm_apply_outputs(&arm_outputs);

  arm_send_text("OK\r\n");
  arm_send_status();
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  arm_board_emergency_stop();
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
