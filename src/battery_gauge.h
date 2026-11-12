/**
 * @file battery_gauge.h
 * @brief 电池电量适配层：读 CW2217、换算百分比、生成业务事件。
 *
 * 只在 main 线程调用。每 RD91_BATT_POLL_MS 做一次短 I2C 事务组（约 1ms），
 * 总线由 bsp/i2c1.c 的互斥锁与 IMU 采样线程/充电轮询串行化。
 * 判定与过期处理在 app/battery_level.c；业务状态仍由 main 线程通过事件队列修改。
 */
#ifndef RD91_BATTERY_GAUGE_H
#define RD91_BATTERY_GAUGE_H

#include <stdint.h>
#include "rd91_types.h"

/* 充电标志“未知”的编码，与 Rd91Event.u.battery.charging 约定一致：
 * 0 = 未充电，1 = 充电中，0xFF = 未知（业务层据此把 charging_valid 置 0）。 */
#define RD91_BATT_CHARGING_UNKNOWN 0xFFU

/* 初始化：确认 VERSION=0xA0 并唤醒电量计。失败只记故障，不影响启动。 */
void BatteryGauge_Init(void);

/*
 * 周期调用；内部按 RD91_BATT_POLL_MS 限速采样。
 * charging 取 0/1/0xFF（来自充电器状态，未知时传 0xFF）。
 * 返回 1 表示 event 已填好（type=RD91_EV_BATTERY），由调用者投递到事件队列；
 * 返回 0 表示本次无需上报。
 */
uint8_t BatteryGauge_Poll(uint8_t charging, Rd91Event *event);

#if RD91_BENCH_BATT_SELFTEST
/*
 * 台架模拟电量：-1 = 回到真实 I2C；0..100 = 模拟百分比。
 * 只由调试器调用（main 线程在断点期间被暂停）。
 */
void Bench_BatterySimSet(int percent);
#endif

#endif /* RD91_BATTERY_GAUGE_H */
