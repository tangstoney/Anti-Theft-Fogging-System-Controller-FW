/* RTT owns SysTick (rt_board.c), PendSV and HardFault (context_gcc.S). */
#include <rtthread.h>
#include "n32wb43x.h"
#include "board.h"
#include "imu_qmi8a01z.h"

void NMI_Handler(void) {}
void MemManage_Handler(void) { for (;;) {} }
void BusFault_Handler(void) { for (;;) {} }
void UsageFault_Handler(void) { for (;;) {} }
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}

/*
 * QMI8A01z 的中断引脚（当前 PC0 / EXTI0）。
 *
 * 这里只做三件事：清 MCU 侧的挂起标志、累加计数、退出中断。
 * 绝对不能在中断里读 I2C：一次寄存器读要几百微秒到毫秒级，
 * 而且 RT-Thread 的互斥量不能在 ISR 里等。真正的 STATUS1 解析放在
 * main 线程里做（见 src/main.c）。
 *
 * 注意 EXTI 挂起标志和芯片 STATUS1 是两回事：
 *   - EXTI 标志：MCU 是否收到过上升沿，清掉后不代表芯片侧条件已撤销；
 *   - STATUS1  ：芯片“此刻是否仍满足运动条件”，由主线程读寄存器得到。
 * 两者都记录，才能在“轻碰一下、条件很快撤销”时仍然知道发生过中断。
 */
void EXTI0_IRQHandler(void)
{
    rt_interrupt_enter();

    if (EXTI_GetITStatus(IMU_INT_EXTI_LINE) != RESET)
    {
        EXTI_ClrITPendBit(IMU_INT_EXTI_LINE);
        Imu_IsrNotify();
    }

    rt_interrupt_leave();
}
