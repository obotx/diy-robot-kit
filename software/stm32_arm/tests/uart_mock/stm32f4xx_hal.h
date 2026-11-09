#ifndef UART_MOCK_H
#define UART_MOCK_H
#include <stdint.h>
typedef struct { volatile unsigned gState; } UART_HandleTypeDef;
enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_UART_STATE_READY, HAL_UART_STATE_BUSY_TX };
int HAL_UART_Transmit_IT(UART_HandleTypeDef *, uint8_t *, uint16_t);
int HAL_UART_AbortTransmit(UART_HandleTypeDef *);
#endif
