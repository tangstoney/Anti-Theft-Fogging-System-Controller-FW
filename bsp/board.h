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

/*
 * 本机声光报警输出（台架接线，实板必须核对）。
 *
 * 选脚依据：避开已占用的 PB5（台架灯）、PA0（按键）、PB8/PB9（I2C1）、
 * PC0（IMU INT）、PB10/PB11（USART3 console）、PA13/PA14（SWD），
 * 以及后续 BLE 预留的 PB14/PB15（UART4）和 PC6/PC7/PC8/PC9（M0 控制）。
 * 下面这些脚在开发板上是否引出、是否被板上外设占用，以到手板原理图为准。
 *
 * WT588F02（语音）：CLK=PB0，DATA=PB1，BUSY=PA1（芯片输出，MCU 输入）。
 *   IO2（两线模式的 CLK）严禁外部上拉：上电为高可能让芯片进入非工作模式。
 *   本工程初始化时把 CLK/DATA 都驱动为低，符合芯片内部 220K 下拉的默认状态。
 * 蜂鸣器：PA2，经外部三极管/MOS 驱动级接有源蜂鸣器（3~36V，≤35mA）。
 * 双色灯：红 PA3、蓝 PA4。接线方式（共阴/共阳/反并联）见
 *   RD91_LAMP_WIRING。
 */
#define ALARM_WT_CLK_PORT   GPIOB
#define ALARM_WT_CLK_PIN    GPIO_PIN_0
#define ALARM_WT_DATA_PORT  GPIOB
#define ALARM_WT_DATA_PIN   GPIO_PIN_1
#define ALARM_WT_BUSY_PORT  GPIOA
#define ALARM_WT_BUSY_PIN   GPIO_PIN_1
#define ALARM_BUZZER_PORT   GPIOA
#define ALARM_BUZZER_PIN    GPIO_PIN_2
#define ALARM_LAMP_RED_PORT GPIOA
#define ALARM_LAMP_RED_PIN  GPIO_PIN_3
#define ALARM_LAMP_BLUE_PORT GPIOA
#define ALARM_LAMP_BLUE_PIN GPIO_PIN_4

void rt_hw_board_init(void);
void Bench_Init(void);
/* 配置 IMU INT 引脚与 EXTI0 中断（不含 IMU 本身）。 */
void Board_ImuIntInit(void);
int Bench_KeyPressed(void);
void Bench_LedToggle(void);
void Bench_SetLed(int on);
uint32_t Bench_NowMs(void);

/* 声光报警三路输出：初始化后全部处于关闭态。 */
void Board_AlarmIoInit(void);
void Board_WtSetClk(int high);
void Board_WtSetData(int high);
/* 返回 1 = BUSY 高（不播放），0 = 低（播放中）。BUSY 未接入时读数无意义。 */
int Board_WtBusyLevel(void);
void Board_BuzzerDrive(int on);
/* 逻辑参数：red_on/blue_on 为 1 表示该颜色点亮；如何换算成电平见 Board_LampDrive。 */
void Board_LampDrive(int red_on, int blue_on);
#endif
