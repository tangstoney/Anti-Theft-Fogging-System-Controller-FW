/**
 * @file battery_level.h
 * @brief 电量计的纯 C 换算与有效性判断：SOC 原始值 -> 百分比、陈旧数据处理。
 *
 * 输入是驱动读到的原始寄存器值（见 drivers/cw2217.h），不含 I2C/RTT 依赖。
 * 这里只做“这颗电量计报告了什么”，不做低电告警阈值：
 * 低电阈值、告警动作属于产品规则（Q11 未确认），不在本模块定义。
 */
#ifndef RD91_BATTERY_LEVEL_H
#define RD91_BATTERY_LEVEL_H

#include <stdint.h>

/* SOC 寄存器上限：100% × 256（低字节 1/256%）。 */
#define RD91_BATTERY_SOC_MAX (100U * 256U)

/* 一次采样的原始值。valid=0 表示本次读取失败或数值超出量程。 */
typedef struct
{
    uint8_t valid;
    uint16_t soc_raw;
    uint16_t vcell_raw;
    uint8_t temp_raw;
} Rd91BatterySample;

/* 判定参数，由适配层从 inc/project_config.h 填入。 */
typedef struct
{
    uint32_t stale_ms; /* 连续多久没有成功读取就把百分比标为未知 */
} Rd91BatteryParams;

/* 判定器状态。 */
typedef struct
{
    uint8_t percent;     /* 最近一次有效百分比，0..100 */
    uint8_t valid;       /* 当前百分比是否可作为有效值使用 */
    uint32_t last_ok_ms; /* 最近一次成功读取的时间 */
} Rd91BatteryState;

void Rd91Battery_Reset(Rd91BatteryState *state);

/*
 * 输入一次采样。返回 1 表示“需要向业务层上报”（有效性或百分比变化），
 * 返回 0 表示无需上报。
 *
 * 读取失败不会立刻把值标成未知：先保留上次有效值，超过 stale_ms 才转为无效，
 * 避免单次 I2C 抖动让显示在 0% 与正常值之间跳动。
 */
uint8_t Rd91Battery_Update(Rd91BatteryState *state, const Rd91BatterySample *sample,
                           uint32_t now_ms, const Rd91BatteryParams *params);

/* SOC 原始值是否在 0..100% 范围内。 */
uint8_t Rd91Battery_SocValid(uint16_t soc_raw);

/* 原始值四舍五入成整数百分比（调用前应确认 SocValid）。 */
uint8_t Rd91Battery_Percent(uint16_t soc_raw);

/* VCELL：1 LSB = 312.5µV -> mV，14 位原始值最大约 5119mV。 */
uint32_t Rd91Battery_VoltageMv(uint16_t vcell_raw);

/* TEMP：1 LSB = 0.5°C，偏移 -40°C；返回 0.1°C 单位。 */
int16_t Rd91Battery_TempTenthsC(uint8_t temp_raw);

#endif /* RD91_BATTERY_LEVEL_H */
