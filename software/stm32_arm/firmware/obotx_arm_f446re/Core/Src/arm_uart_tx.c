#include "arm_uart_tx.h"

ArmTxStatus arm_uart_tx_poll(ArmUartTx *tx, UART_HandleTypeDef *uart, uint32_t now)
{
    if (!tx->active) return ARM_TX_IDLE;
    if (uart->gState == HAL_UART_STATE_READY) {
        tx->active = false;
        return ARM_TX_IDLE;
    }
    if ((uint32_t)(now - tx->started_ms) >= 90U) {
        HAL_UART_AbortTransmit(uart);
        tx->active = false;
        return ARM_TX_FAILED;
    }
    return ARM_TX_BUSY;
}

bool arm_uart_tx_start(ArmUartTx *tx, UART_HandleTypeDef *uart,
                       uint8_t *data, uint16_t length, uint32_t now)
{
    if (tx->active || !length) return false;
    if (HAL_UART_Transmit_IT(uart, data, length) != HAL_OK) return false;
    tx->started_ms = now;
    tx->active = true;
    return true;
}
