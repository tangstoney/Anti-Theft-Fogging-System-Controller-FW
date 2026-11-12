/**
 * @file charge_indicator.h
 * @brief 充电/状态指示灯的纯 C 判定：把 SGM41513 状态寄存器变成灯色。
 *
 * 输入是驱动解码后的状态位（见 drivers/sgm41513.h），不是 I2C 原始字节，
 * 也不是电量。以后换电量计或改用其它充电器，只要适配层能给出同样的字段，
 * 这里的去抖与灯色策略不用改。
 *
 * 判定规则（SGM41513 数据手册 REG08 / REG09）：
 *   PG_STAT=0               -> 没有可用输入，灯灭；
 *   CHRG_STAT=01/10         -> 预充/快充，红灯；
 *   CHRG_STAT=11            -> 充电完成，绿灯；
 *   CHRG_STAT=00            -> 有输入但充电被禁用，灯灭（待产品确认）；
 *   CHRG_FAULT!=00 / BAT_FAULT / NTC 过冷过热 -> 故障，红灯闪烁。
 *
 * 故障用寄存器里的“当前故障”位判定，不靠 STAT 引脚翻转；
 * 正常充电过程不会有故障位，充电完成只是 CHRG_STAT 变 11。
 */
#ifndef RD91_CHARGE_INDICATOR_H
#define RD91_CHARGE_INDICATOR_H

#include <stdint.h>
#include "rd91_types.h"

/* 一次采样的解码结果。valid=0 表示本次读取失败，不能据此改变灯色。 */
typedef struct
{
    uint8_t valid;
    uint8_t pg_stat;    /* 1 = 输入电源正常 */
    uint8_t chrg_stat;  /* 0 禁用, 1 预充, 2 快充, 3 充满 */
    uint8_t chrg_fault; /* 0 正常, 1 输入故障, 2 过热, 3 安全定时器超时 */
    uint8_t bat_fault;  /* 1 = 电池过压 */
    uint8_t ntc_fault;  /* 0 正常, 2 暖, 3 凉, 5 冷, 6 热（JEITA） */
} Rd91ChargeSample;

/* 判定参数。由适配层从 inc/project_config.h 填入，模块本身不含阈值。 */
typedef struct
{
    uint32_t debounce_ms;    /* 候选状态保持多久才切换 */
    uint32_t fault_blink_ms; /* 故障时红灯亮/灭各持续多久 */
} Rd91ChargeParams;

/* 判定器内部状态。调用者只负责初始化与逐次采样。 */
typedef struct
{
    uint8_t state;            /* Rd91ChargeState，已去抖 */
    uint8_t candidate;        /* 原始判定，等待去抖确认 */
    uint32_t candidate_since; /* 候选状态开始时间 */
} Rd91ChargeJudge;

void Rd91Charge_Reset(Rd91ChargeJudge *judge);

/* 输入一次解码后的采样。返回 1 表示去抖后的状态发生了变化。 */
uint8_t Rd91Charge_Update(Rd91ChargeJudge *judge, const Rd91ChargeSample *sample,
                          uint32_t now_ms, const Rd91ChargeParams *params);

/*
 * 当前状态对应的灯色（逻辑量，1 = 点亮）：
 *   充电中        -> 红
 *   充满          -> 绿
 *   故障          -> 红 blink_ms 亮/灭交替
 *   无输入/禁用/未知 -> 全灭
 */
void Rd91Charge_LedLevels(uint8_t state, const Rd91ChargeParams *params,
                          uint32_t now_ms, uint8_t *red_on, uint8_t *green_on);

#endif /* RD91_CHARGE_INDICATOR_H */
