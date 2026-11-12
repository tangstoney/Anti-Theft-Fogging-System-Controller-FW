/**
 * @file alarm_output.h
 * @brief 本机声光报警服务：语音循环、蜂鸣器常响、红蓝灯交替。
 *
 * 调用约定：
 *   - AlarmOut_Init() 在系统启动时调用一次，之后三路输出处于关闭态；
 *   - AlarmOut_SetOutput() 只登记“期望状态”，可被任何线程调用，不阻塞、不等待；
 *   - 真正驱动 GPIO/语音芯片的只有一个静态线程，见 alarm_output.c。
 */
#ifndef RD91_ALARM_OUTPUT_H
#define RD91_ALARM_OUTPUT_H

#include <stdint.h>
#include "rd91_types.h"

/* 初始化三路输出（先关断）、事件对象与声光线程。 */
void AlarmOut_Init(void);

/* 登记某一路输出的期望状态（output 为 Rd91Output，on = 0/1）。
 * 重复调用幂等：只有状态真正变化时才递增版本并唤醒线程。 */
void AlarmOut_SetOutput(uint8_t output, uint8_t on);

/* 读取最近一次已发布的快照（可为 NULL）。 */
void AlarmOut_GetSnapshot(Rd91AlarmSnapshot *snapshot);

/* 预留的通知钩子。回调在声光线程上下文执行，必须非阻塞，并自行复制快照。
 * 未来 BLE 线程应在这里保存自己的待发送记录，不能与其它订阅者共用一份。 */
typedef void (*Rd91AlarmNotify)(const Rd91AlarmSnapshot *snapshot);

/* 注册通知回调；传 NULL 取消。当前主程序不注册（无线未实现）。 */
void AlarmOut_SetNotify(Rd91AlarmNotify notify);

#endif /* RD91_ALARM_OUTPUT_H */
