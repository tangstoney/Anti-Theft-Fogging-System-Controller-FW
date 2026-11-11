/**
 * @file n32wb43x_it.h
 * @brief 中断处理函数声明。
 */
#ifndef N32WB43X_IT_H
#define N32WB43X_IT_H

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void SysTick_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* N32WB43X_IT_H */
