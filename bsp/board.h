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
 *   单线模式（RD91_WT_WIRE_MODE=1）：数据线是芯片 IO2，接 PB0；PB1（IO3）不用。
 *   两线模式（=2）：CLK=IO2/PB0，DATA=IO3/PB1。
 *   IO2 严禁外部上拉：上电为高可能让芯片进入非工作模式。
 *   本工程初始化时把 PB0/PB1 都驱动为低，符合芯片内部 220K 下拉的默认状态。
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

/*
 * 充电/状态指示灯（台架接线，实板必须核对）。
 *
 * 与报警灯是两套独立输出：报警红蓝灯归声光线程，这盏红绿灯归 main 线程。
 * 选脚继续避开已占用/预留的 PA0、PA1~PA4、PB0/PB1、PB5、PB8/PB9、PC0、
 * PB10/PB11、PA13/PA14、PB14/PB15、PC6~PC9。
 *
 * 充电器状态不走 GPIO：SGM41513 与电量计都接在 I2C 上（STAT/nPG 是否
 * 引出未确认，nPG 预计不接），由 drivers/sgm41513.c 读 REG08/REG09。
 * 这里只保留双色灯：
 *   红灯 -> PA5，绿灯 -> PA6；接线方式见 RD91_CHG_LED_WIRING。
 */
#define CHARGE_LED_RED_PORT  GPIOA
#define CHARGE_LED_RED_PIN   GPIO_PIN_5
#define CHARGE_LED_GREEN_PORT GPIOA
#define CHARGE_LED_GREEN_PIN GPIO_PIN_6

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
/* 单线模式的数据线（芯片 IO2，与两线 CLK 同一脚，驱动 PB0）。 */
void Board_WtSetWire(int high);
/* 返回 1 = BUSY 高（不播放），0 = 低（播放中）。BUSY 未接入时读数无意义。 */
int Board_WtBusyLevel(void);
void Board_BuzzerDrive(int on);
/* 逻辑参数：red_on/blue_on 为 1 表示该颜色点亮；如何换算成电平见 Board_LampDrive。 */
void Board_LampDrive(int red_on, int blue_on);

/* 充电指示灯：初始化后全灭。状态从 I2C 充电器读取，不占用 GPIO 输入。 */
void Board_ChargeIoInit(void);
/* 逻辑参数：red_on/green_on 为 1 表示该颜色点亮；引脚电平由 RD91_CHG_LED_WIRING 决定。 */
void Board_ChargeLedDrive(int red_on, int green_on);
#endif
