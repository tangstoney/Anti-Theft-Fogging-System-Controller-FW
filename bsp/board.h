#ifndef RD91_BOARD_H
#define RD91_BOARD_H
#include <stdint.h>
#include "n32wb43x.h"
#define BENCH_LED_PORT GPIOB
#define BENCH_LED_PIN GPIO_PIN_5
#define BENCH_LED_CLOCK RCC_APB2_PERIPH_GPIOB
#define BENCH_LED_ACTIVE_LOW 0
#define BENCH_KEY_PORT GPIOA
#define BENCH_KEY_PIN GPIO_PIN_0
#define BENCH_KEY_CLOCK RCC_APB2_PERIPH_GPIOA

/* QMI8A01z INT1 接线：PC0 / EXTI0（.ai/board.md 约定）。
 * 芯片 INT 引脚在 CTRL1.INT1_EN=1 时是推挽输出，事件发生时拉高，
 * 所以这里用内部下拉 + 上升沿触发。若到手模块把 INT 接到了别的脚，
 * 改下面 6 行，并在 bsp/n32wb43x_it.c 里把中断函数名改成对应的 EXTIx_IRQHandler。 */
#define IMU_INT_PORT GPIOC
#define IMU_INT_PIN GPIO_PIN_0
#define IMU_INT_CLOCK RCC_APB2_PERIPH_GPIOC
#define IMU_INT_PORT_SOURCE GPIOC_PORT_SOURCE
#define IMU_INT_PIN_SOURCE GPIO_PIN_SOURCE0
#define IMU_INT_EXTI_LINE EXTI_LINE0
#define IMU_INT_IRQn EXTI0_IRQn

void rt_hw_board_init(void);
void Bench_Init(void);
/* 配置 IMU INT 引脚与 EXTI0 中断（不含 IMU 本身）。 */
void Board_ImuIntInit(void);
int Bench_KeyPressed(void);
void Bench_LedToggle(void);
void Bench_SetLed(int on);
uint32_t Bench_NowMs(void);
#endif
