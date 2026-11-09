#ifndef ARM_UART_TX_H
#define ARM_UART_TX_H
#include "stm32f4xx_hal.h"
#include <stdbool.h>

typedef struct { bool active; uint32_t started_ms; } ArmUartTx;
typedef enum { ARM_TX_IDLE, ARM_TX_BUSY, ARM_TX_FAILED } ArmTxStatus;
/* Caller retains the buffer unchanged until poll returns IDLE or FAILED. */
ArmTxStatus arm_uart_tx_poll(ArmUartTx *tx, UART_HandleTypeDef *uart, uint32_t now);
bool arm_uart_tx_start(ArmUartTx *tx, UART_HandleTypeDef *uart,
                       uint8_t *data, uint16_t length, uint32_t now);
#endif
