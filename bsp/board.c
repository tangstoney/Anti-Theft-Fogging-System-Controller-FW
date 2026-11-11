#include <rtthread.h>
#include "n32wb43x.h"
#include "board.h"

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
