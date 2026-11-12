#include <rtthread.h>
#include "n32wb43x.h"
#include "board.h"
#include "project_config.h"

/* Current 100Hz config divides 1000 exactly, preserving uint32_t modulo time. */
#if RT_TICK_PER_SECOND > 1000 || (1000 % RT_TICK_PER_SECOND) != 0
#error Update Bench_NowMs when changing to a tick rate that does not divide 1000
#endif
uint32_t Bench_NowMs(void)
{
    return (uint32_t)rt_tick_get() * (1000U / RT_TICK_PER_SECOND);
}
void Bench_Init(void)
{
    GPIO_InitType gpio;
    RCC_EnableAPB2PeriphClk(BENCH_LED_CLOCK | BENCH_KEY_CLOCK, ENABLE);
    /* Preload inactive LED level before enabling output. */
    if (BENCH_LED_ACTIVE_LOW) { GPIO_SetBits(BENCH_LED_PORT, BENCH_LED_PIN); }
    else { GPIO_ResetBits(BENCH_LED_PORT, BENCH_LED_PIN); }
    GPIO_InitStruct(&gpio);
    gpio.Pin = BENCH_LED_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pull = GPIO_No_Pull;
    gpio.GPIO_Current = GPIO_DC_4mA;
    GPIO_InitPeripheral(BENCH_LED_PORT, &gpio);
    GPIO_InitStruct(&gpio);
    gpio.Pin = BENCH_KEY_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Input;
    gpio.GPIO_Pull = GPIO_Pull_Up;
    GPIO_InitPeripheral(BENCH_KEY_PORT, &gpio);
}
int Bench_KeyPressed(void)
{
    return GPIO_ReadInputDataBit(BENCH_KEY_PORT, BENCH_KEY_PIN) == Bit_RESET;
}
void Bench_LedToggle(void)
{
    BENCH_LED_PORT->POD ^= BENCH_LED_PIN;
}

void Bench_SetLed(int on)
{
    if (BENCH_LED_ACTIVE_LOW)
    {
        on = !on;
    }
    if (on)
    {
        GPIO_SetBits(BENCH_LED_PORT, BENCH_LED_PIN);
    }
    else
    {
        GPIO_ResetBits(BENCH_LED_PORT, BENCH_LED_PIN);
    }
}

void Board_ImuIntInit(void)
{
    GPIO_InitType gpio;
    EXTI_InitType exti;

    RCC_EnableAPB2PeriphClk(IMU_INT_CLOCK, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);

    /* 输入 + 下拉：IMU 的 INT 在未使能时是高阻态，下拉保证读到的电平确定。 */
    GPIO_InitStruct(&gpio);
    gpio.Pin = IMU_INT_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Input;
    gpio.GPIO_Pull = GPIO_Pull_Down;
    GPIO_InitPeripheral(IMU_INT_PORT, &gpio);

    /* AFIO 把 EXTI 线连到具体端口。漏掉这句会让中断永远来自错误引脚。 */
    GPIO_ConfigEXTILine(IMU_INT_PORT_SOURCE, IMU_INT_PIN_SOURCE);

    /* 上升沿触发：手册 §6 写明 INT 引脚为推挽、事件时“拉高、读寄存器后拉低”。 */
    EXTI_InitStruct(&exti);
    exti.EXTI_Line = IMU_INT_EXTI_LINE;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_InitPeripheral(&exti);

    /* 优先级低于 RTT 内核相关中断，ISR 只做计数，不会影响调度。 */
    NVIC_SetPriority(IMU_INT_IRQn, 10);
    NVIC_EnableIRQ(IMU_INT_IRQn);
}

/* ---------- 本机声光报警输出 ---------- */

/* 逻辑电平 -> 引脚电平。RD91_LAMP_WIRING 在 inc/project_config.h 集中配置。 */
void Board_LampDrive(int red_on, int blue_on)
{
#if RD91_LAMP_WIRING == 0
    /* 3 脚共阴：高电平点亮。 */
    if (red_on)  { GPIO_SetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN); }
    else         { GPIO_ResetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN); }
    if (blue_on) { GPIO_SetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN); }
    else         { GPIO_ResetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN); }
#elif RD91_LAMP_WIRING == 1
    /* 3 脚共阳：低电平点亮。 */
    if (red_on)  { GPIO_ResetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN); }
    else         { GPIO_SetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN); }
    if (blue_on) { GPIO_ResetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN); }
    else         { GPIO_SetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN); }
#elif RD91_LAMP_WIRING == 2
    /* 2 脚反并联：两脚反相才能点亮一种颜色；同时请求按红色处理（本工程不会这样用）。 */
    if (red_on && !blue_on)
    {
        GPIO_SetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN);
        GPIO_ResetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN);
    }
    else if (!red_on && blue_on)
    {
        GPIO_ResetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN);
        GPIO_SetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN);
    }
    else
    {
        GPIO_ResetBits(ALARM_LAMP_RED_PORT, ALARM_LAMP_RED_PIN);
        GPIO_ResetBits(ALARM_LAMP_BLUE_PORT, ALARM_LAMP_BLUE_PIN);
    }
#else
#error Unsupported RD91_LAMP_WIRING
#endif
}

void Board_BuzzerDrive(int on)
{
#if RD91_BUZZER_ACTIVE_HIGH
    if (on) { GPIO_SetBits(ALARM_BUZZER_PORT, ALARM_BUZZER_PIN); }
    else    { GPIO_ResetBits(ALARM_BUZZER_PORT, ALARM_BUZZER_PIN); }
#else
    if (on) { GPIO_ResetBits(ALARM_BUZZER_PORT, ALARM_BUZZER_PIN); }
    else    { GPIO_SetBits(ALARM_BUZZER_PORT, ALARM_BUZZER_PIN); }
#endif
}

void Board_WtSetClk(int high)
{
    if (high) { GPIO_SetBits(ALARM_WT_CLK_PORT, ALARM_WT_CLK_PIN); }
    else      { GPIO_ResetBits(ALARM_WT_CLK_PORT, ALARM_WT_CLK_PIN); }
}

void Board_WtSetData(int high)
{
    if (high) { GPIO_SetBits(ALARM_WT_DATA_PORT, ALARM_WT_DATA_PIN); }
    else      { GPIO_ResetBits(ALARM_WT_DATA_PORT, ALARM_WT_DATA_PIN); }
}

/*
 * 单线模式的数据线：WT588F02 的单线数据输入端是 IO2（pin2），
 * 与两线模式的 CLK 是同一个脚，因此驱动 PB0。PB1（两线 DATA/IO3）
 * 在单线模式下不连接也不使用，初始化后保持低电平。
 */
void Board_WtSetWire(int high)
{
    if (high) { GPIO_SetBits(ALARM_WT_CLK_PORT, ALARM_WT_CLK_PIN); }
    else      { GPIO_ResetBits(ALARM_WT_CLK_PORT, ALARM_WT_CLK_PIN); }
}

int Board_WtBusyLevel(void)
{
    return (GPIO_ReadInputDataBit(ALARM_WT_BUSY_PORT, ALARM_WT_BUSY_PIN) == Bit_SET) ? 1 : 0;
}

void Board_AlarmIoInit(void)
{
    GPIO_InitType gpio;

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA | RCC_APB2_PERIPH_GPIOB, ENABLE);

    /*
     * 先把输出寄存器预置成非活动电平，再切到输出模式：
     * 避免初始化瞬间蜂鸣器响一下、灯闪一下。
     * WT 的 PB0/PB1 保持低：单线模式下 PB0 是数据线（芯片 IO2），
     * 资料要求 IO2 不得上拉，上电为高可能进入非工作模式；
     * 两线模式下 PB0/PB1 分别是 CLK/DATA，空闲低同样安全。
     */
    GPIO_ResetBits(ALARM_WT_CLK_PORT, ALARM_WT_CLK_PIN);
    GPIO_ResetBits(ALARM_WT_DATA_PORT, ALARM_WT_DATA_PIN);
    Board_BuzzerDrive(0);
    Board_LampDrive(0, 0);

    GPIO_InitStruct(&gpio);
    gpio.Pin = ALARM_WT_CLK_PIN | ALARM_WT_DATA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pull = GPIO_No_Pull;
    gpio.GPIO_Current = GPIO_DC_4mA;
    GPIO_InitPeripheral(ALARM_WT_CLK_PORT, &gpio);

    GPIO_InitStruct(&gpio);
    gpio.Pin = ALARM_BUZZER_PIN | ALARM_LAMP_RED_PIN | ALARM_LAMP_BLUE_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pull = GPIO_No_Pull;
    gpio.GPIO_Current = GPIO_DC_4mA;
    GPIO_InitPeripheral(ALARM_BUZZER_PORT, &gpio);

    /*
     * BUSY 是芯片的推挽输出（播放低、空闲高），MCU 侧只做输入。
     * 内部上拉只在 WT 用 3.3V 供电且直连时成立：芯片缺失时读高=“空闲”，
     * 等待播放的检查会超时并报故障，而不是误判成已播放。
     * 若 WT 用 5V 供电，BUSY 高电平约 0.95×5V，必须先分压/电平转换再进 MCU。
     */
    GPIO_InitStruct(&gpio);
    gpio.Pin = ALARM_WT_BUSY_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Input;
    gpio.GPIO_Pull = GPIO_Pull_Up;
    GPIO_InitPeripheral(ALARM_WT_BUSY_PORT, &gpio);
}

/* ---------- 充电/状态指示灯 ---------- */

/* 逻辑电平 -> 引脚电平。RD91_CHG_LED_WIRING 在 inc/project_config.h 集中配置。 */
void Board_ChargeLedDrive(int red_on, int green_on)
{
#if RD91_CHG_LED_WIRING == 0
    /* 3 脚共阴：高电平点亮。 */
    if (red_on)   { GPIO_SetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN); }
    else          { GPIO_ResetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN); }
    if (green_on) { GPIO_SetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN); }
    else          { GPIO_ResetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN); }
#elif RD91_CHG_LED_WIRING == 1
    /* 3 脚共阳：低电平点亮。 */
    if (red_on)   { GPIO_ResetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN); }
    else          { GPIO_SetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN); }
    if (green_on) { GPIO_ResetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN); }
    else          { GPIO_SetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN); }
#elif RD91_CHG_LED_WIRING == 2
    /* 2 脚反并联：两脚反相才能点亮一种颜色；同时请求按红色处理。 */
    if (red_on && !green_on)
    {
        GPIO_SetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN);
        GPIO_ResetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN);
    }
    else if (!red_on && green_on)
    {
        GPIO_ResetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN);
        GPIO_SetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN);
    }
    else
    {
        GPIO_ResetBits(CHARGE_LED_RED_PORT, CHARGE_LED_RED_PIN);
        GPIO_ResetBits(CHARGE_LED_GREEN_PORT, CHARGE_LED_GREEN_PIN);
    }
#else
#error Unsupported RD91_CHG_LED_WIRING
#endif
}

void Board_ChargeIoInit(void)
{
    GPIO_InitType gpio;

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA, ENABLE);

    /* 先把输出寄存器预置成“全灭”，再切到输出模式，避免上电闪一下。 */
    Board_ChargeLedDrive(0, 0);

    GPIO_InitStruct(&gpio);
    gpio.Pin = CHARGE_LED_RED_PIN | CHARGE_LED_GREEN_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pull = GPIO_No_Pull;
    gpio.GPIO_Current = GPIO_DC_4mA;
    GPIO_InitPeripheral(CHARGE_LED_RED_PORT, &gpio);
}
