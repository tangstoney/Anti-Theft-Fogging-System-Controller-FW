/**
 * @file charge_led.h
 * @brief 充电/状态指示灯适配层：读 SGM41513 状态寄存器，驱动红绿双色灯。
 *
 * 只在 main 线程调用。每 RD91_CHG_POLL_MS 做一次短 I2C 事务（<1ms），
 * 总线由 bsp/i2c1.c 的互斥锁与 IMU 采样线程串行化。
 * 判定逻辑在 app/charge_indicator.c。
 */
#ifndef RD91_CHARGE_LED_H
#define RD91_CHARGE_LED_H

#include <stdint.h>
#include "project_config.h"

/* 初始化灯引脚与判定器，灯先全灭；不在位/总线不可用时只记故障，不影响启动。 */
void ChargeLed_Init(void);

/* main 循环里周期调用；内部按 RD91_CHG_POLL_MS 限速采样。 */
void ChargeLed_Poll(void);

/* 最近一次去抖后的充电器状态（Rd91ChargeState），供显示/诊断使用。 */
uint8_t ChargeLed_GetState(void);

#if RD91_BENCH_CHG_SELFTEST
/*
 * 台架模拟充电器状态，供没有充电模块时验证灯色：
 *   -1 = 回到真实 I2C；0 = 无输入；1 = 充电中；2 = 充满；
 *    3 = 充电故障（过热）；4 = 有输入但充电被禁用。
 * 只由调试器调用（main 线程在断点期间被暂停）；不要在其他线程里调用。
 */
void Bench_ChargeSimSet(int state);
#endif

#endif /* RD91_CHARGE_LED_H */
