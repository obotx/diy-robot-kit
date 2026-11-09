#ifndef HW_MOCK_MAIN_H
#define HW_MOCK_MAIN_H
#include <stdint.h>
typedef struct { volatile uint32_t CR1,CR2,SMCR,DIER,SR,EGR,CCMR1,CCER,CNT,PSC,ARR,CCR1,CCR2,CCMR2,CCR3; } TIM_TypeDef;
extern TIM_TypeDef mock_tim4;
#define TIM4 (&mock_tim4)
#define GPIOB (&mock_gpio)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_9 (1U << 9)
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_SPEED_FREQ_LOW 0U
#define GPIO_AF2_TIM4 2U
#define TIM_CCMR2_OC3M_Pos 4U
#define TIM_CCMR2_OC3PE (1U << 3)
#define TIM_CCER_CC3E (1U << 8)
#define TIM_CR1_ARPE (1U << 7)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM4_CLK_ENABLE() ((void)0)
typedef struct { uint32_t pins; } GPIO_TypeDef;
typedef struct { uint32_t Pin,Mode,Pull,Speed,Alternate; } GPIO_InitTypeDef;
typedef struct { uint32_t APB1CLKDivider; } RCC_ClkInitTypeDef;
typedef struct { volatile uint32_t CR,NDTR; volatile uintptr_t PAR,M0AR; volatile uint32_t FCR; } DMA_Stream_TypeDef;
typedef struct { volatile uint32_t LISR,LIFCR; } DMA_TypeDef;
extern DMA_Stream_TypeDef mock_dma_streams[3];
extern DMA_TypeDef mock_dma;
#define DMA1 (&mock_dma)
#define DMA1_Stream0 (&mock_dma_streams[0])
#define DMA1_Stream1 (&mock_dma_streams[1])
#define DMA1_Stream2 (&mock_dma_streams[2])
#define DMA_SxCR_EN 1U
#define DMA_SxCR_TCIE (1U << 4)
#define DMA_SxCR_TEIE (1U << 2)
#define DMA_SxCR_DMEIE (1U << 1)
#define DMA1_Stream0_IRQn 10
#define DMA1_Stream1_IRQn 11
#define DMA1_Stream2_IRQn 12
#define DMA_SxCR_CHSEL_Pos 25U
#define DMA_SxCR_DIR_0 (1U << 6)
#define DMA_SxCR_MINC (1U << 10)
#define DMA_SxCR_PSIZE_1 (1U << 12)
#define DMA_SxCR_MSIZE_1 (1U << 14)
#define DMA_SxCR_PL (3U << 16)
#define TIM_DIER_UDE (1U << 8)
#define __HAL_RCC_DMA1_CLK_ENABLE() ((void)0)
#ifdef ARM_DMA_START_CHECK
void mock_dma_start_check(void);
#define __DMB() mock_dma_start_check()
#else
#define __DMB() ((void)0)
#endif
uint32_t HAL_GetTick(void);
extern TIM_TypeDef mock_tim2,mock_tim3,mock_tim5;
#ifdef ARM_STEP_ACCOUNT_CHECK
void arm_step_account_check(TIM_TypeDef *timer);
#endif
extern GPIO_TypeDef mock_gpio;
#define TIM2 (&mock_tim2)
#define TIM3 (&mock_tim3)
#define TIM5 (&mock_tim5)
#define GPIOA (&mock_gpio)
#define ZL_DIR_GPIO_Port (&mock_gpio)
#define ZR_DIR_GPIO_Port (&mock_gpio)
#define X_DIR_GPIO_Port (&mock_gpio)
#define ZL_DIR_Pin 1U
#define ZR_DIR_Pin 2U
#define X_DIR_Pin 4U
#define GPIO_PIN_1 2U
#define GPIO_PIN_SET 1U
#define GPIO_PIN_RESET 0U
#define GPIO_MODE_AF_PP 0U
#define GPIO_PULLDOWN 0U
#define GPIO_SPEED_FREQ_HIGH 0U
#define GPIO_AF2_TIM5 2U
#define RCC_HCLK_DIV1 1U
#define TIM_SR_UIF 1U
#define TIM_EGR_UG 1U
#define TIM_DIER_UIE 1U
#define TIM_CR1_CEN 1U
#define TIM_CR1_URS 4U
#define TIM_CR1_OPM 8U
#define TIM_CCER_CC1E 1U
#define TIM_CCER_CC2E 16U
#define TIM_CCMR1_OC1M_Pos 4U
#define TIM_CCMR1_OC2M_Pos 12U
#define TIM2_IRQn 2
#define TIM3_IRQn 3
#define TIM5_IRQn 5
#define __HAL_RCC_TIM2_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM3_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM5_CLK_ENABLE() ((void)0)
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __disable_irq(void) {}
static inline void __set_PRIMASK(uint32_t v) { (void)v; }
static inline void HAL_GPIO_WritePin(GPIO_TypeDef *g,uint16_t p,uint32_t v) { if (v) g->pins|=p; else g->pins&=~p; }
static inline void HAL_GPIO_Init(GPIO_TypeDef *g,GPIO_InitTypeDef *v) { (void)g; (void)v; }
static inline void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef *c,uint32_t *l) { c->APB1CLKDivider=2; *l=0; }
static inline uint32_t HAL_RCC_GetPCLK1Freq(void) { return 42000000; }
static inline void HAL_NVIC_SetPriority(int n,int p,int s) { (void)n; (void)p; (void)s; }
static inline void HAL_NVIC_EnableIRQ(int n) { (void)n; }
#endif
