#ifdef NDEBUG
#undef NDEBUG
#endif
#include "arm_uart_tx.h"
#include <assert.h>
#include <stdio.h>
static unsigned starts, aborts;
static int result = HAL_OK;
static uint8_t *pending;
int HAL_UART_Transmit_IT(UART_HandleTypeDef *uart, uint8_t *data, uint16_t length) {
    assert(length == 3);
    ++starts;
    if (result == HAL_OK) { uart->gState = HAL_UART_STATE_BUSY_TX; pending = data; }
    return result;
}
int HAL_UART_AbortTransmit(UART_HandleTypeDef *uart) {
    ++aborts; uart->gState = HAL_UART_STATE_READY; return HAL_OK;
}
int main(void) {
    ArmUartTx tx = {0};
    UART_HandleTypeDef uart = {HAL_UART_STATE_READY};
    uint8_t reply[] = "ok\n";
    assert(arm_uart_tx_start(&tx, &uart, reply, 3, 100));
    assert(!arm_uart_tx_start(&tx, &uart, reply, 3, 101));
    /* Motion ticks can continue throughout a reply. No blocking HAL call. */
    for (uint32_t now=100; now<180; now+=10)
        assert(arm_uart_tx_poll(&tx, &uart, now)==ARM_TX_BUSY);
    assert(starts==1 && aborts==0 && pending==reply);
    uart.gState=HAL_UART_STATE_READY;
    assert(arm_uart_tx_poll(&tx,&uart,180)==ARM_TX_IDLE);
    assert(arm_uart_tx_start(&tx,&uart,reply,3,UINT32_MAX-40));
    assert(arm_uart_tx_poll(&tx,&uart,48)==ARM_TX_BUSY);
    assert(arm_uart_tx_poll(&tx,&uart,49)==ARM_TX_FAILED);
    assert(aborts==1 && !tx.active);
    result=HAL_BUSY;
    assert(!arm_uart_tx_start(&tx,&uart,reply,3,200) && !tx.active);
    result=HAL_OK;
    assert(arm_uart_tx_start(&tx,&uart,reply,3,210));
    puts("PASS nonblocking UART TX: buffer ownership, completion, timeout, tick wrap, HAL failure");
}
