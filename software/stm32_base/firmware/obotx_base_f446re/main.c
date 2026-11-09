#include "stm32f4xx_hal.h"
#include "base_link.h"

#include "base_board.h"
#include "base_drive_config.h"
#define RX_SIZE 4096U
static BaseLink link;
static BaseDrive drive;
static volatile bool tx_busy, control_trip;
static volatile uint32_t control_ms;
static UART_HandleTypeDef uart;
static uint8_t rx_byte, rx_ring[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile bool rx_error, started;
static char reply[BASE_LINK_TX_SIZE];
int base_board_identity(char device_id[81], char boot_id[81]);

static void outputs_off(void)
{
    base_board_off();
}

static void halt(void)
{
    __disable_irq();
    outputs_off();
    for (;;) {}
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    if (started) {
        uint32_t now = HAL_GetTick();
        if ((uint32_t)(now - control_ms) > 30U) control_trip = true;
        if (base_link_watchdog_isr(&link, now) ||
            control_trip || rx_error) outputs_off();
    }
}
void HardFault_Handler(void) { halt(); }
void MemManage_Handler(void) { halt(); }
void BusFault_Handler(void) { halt(); }
void UsageFault_Handler(void) { halt(); }
void NMI_Handler(void) { halt(); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&uart); }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *handle)
{
    if (handle != &uart) return;
    uint32_t next = (rx_head + 1U) % RX_SIZE;
    if (next == rx_tail) rx_error = true;
    else if (!rx_error) {
        rx_ring[rx_head] = rx_byte;
        __DMB();
        rx_head = next;
    }
    if (HAL_UART_Receive_IT(&uart, &rx_byte, 1U) != HAL_OK) rx_error = true;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *handle)
{
    if (handle == &uart) tx_busy = false;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *handle)
{
    if (handle == &uart) rx_error = true;
}

static void init_gpio(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    base_board_init();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);
}

int main(void)
{
    /* Use reset HSI at 16 MHz, independent of ST-LINK's external clock. */
    HAL_Init();
    init_gpio();
    char device_id[81], boot_id[81];
    if (!base_board_identity(device_id, boot_id)) halt();
    base_link_init(&link, device_id, boot_id);
    base_drive_init(&drive, &BASE_DRIVE_CONFIG);
    link.drive = &drive;
    __HAL_RCC_USART2_CLK_ENABLE();
    uart.Instance = USART2;
    uart.Init.BaudRate = 115200;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.Mode = UART_MODE_TX_RX;
    uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&uart) != HAL_OK) halt();
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    control_ms = HAL_GetTick();
    started = true;
    if (HAL_UART_Receive_IT(&uart, &rx_byte, 1U) != HAL_OK) halt();
    for (;;) {
        uint32_t now = HAL_GetTick();
        if (control_trip) base_drive_fault(&drive, "control_tick_overrun");
        base_link_tick(&link, now);
        if ((uint32_t)(now - control_ms) >= 10U) {
            int32_t delta[4];
            base_board_counts(delta);
            uint32_t dt = now - control_ms;
            base_drive_tick(&drive, delta, dt);
            uint32_t mask = __get_PRIMASK(); __disable_irq();
            control_ms = now;
            if (drive.enabled && link.session_valid && !link.watchdog_expired &&
                !rx_error && !control_trip)
                base_board_apply(drive.pwm);
            else outputs_off();
            __set_PRIMASK(mask);
        }
        if (!drive.enabled) outputs_off();
        if (rx_error) {
            HAL_NVIC_DisableIRQ(USART2_IRQn);
            outputs_off();
            HAL_UART_Abort(&uart);
            tx_busy = false;
            __HAL_UART_CLEAR_OREFLAG(&uart);
            rx_tail = rx_head;
            rx_error = false;
            base_link_abort(&link, "uart_error");
            link.rx_len = 0;
            link.dropping = true; /* Resynchronize only at a newline. */
            if (HAL_UART_Receive_IT(&uart, &rx_byte, 1U) != HAL_OK) halt();
            HAL_NVIC_EnableIRQ(USART2_IRQn);
        }
        if (!tx_busy && rx_tail != rx_head) {
            uint8_t byte = rx_ring[rx_tail];
            __DMB();
            rx_tail = (rx_tail + 1U) % RX_SIZE;
            size_t size = base_link_feed(&link, HAL_GetTick(), byte, reply, sizeof(reply));
            if (!drive.enabled) outputs_off();
            if (size) {
                tx_busy = true;
                if (HAL_UART_Transmit_IT(&uart, (uint8_t *)reply, (uint16_t)size) != HAL_OK) {
                    tx_busy = false;
                    base_link_abort(&link, "uart_tx_error");
                    outputs_off();
                }
            }
        }
    }
}

