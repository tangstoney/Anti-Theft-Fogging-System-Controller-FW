/**
 * @file shock_detect.h
 * @brief 疑似冲击链路的纯 C 实现：逐样本检测、峰值保持与事件聚合。
 *
 * 指标（变量名里写清楚，避免和别的量混淆）：
 *   norm_mg = 三轴合矢量 |a| = sqrt(ax^2 + ay^2 + az^2)，单位 mg。
 *   静止放置时 |a| ≈ 1000mg（重力本身）。进入阈值 2000mg 表示总模长 2.0g。
 *
 * 为什么选“总模长”而不是“相对某个参考向量的变化量”：
 *   - 与安装姿态无关：车辆怎么装、模块怎么贴，静止时都是约 1g；
 *   - 容易解释和复现，台架上用手轻敲就能看到数值变化；
 *   - 局限：|a| 含重力，它不是“冲击加速度本身”，也不能区分方向。
 *     相对参考向量的变化量需要先定一个“静止参考”，会引入姿态/标定问题，
 *     本轮不采用。
 *
 * 本模块不做产品判断：输出只是“疑似冲击”，不是砸车/碰撞结论。
 * 缓慢撬动可能没有大尖峰，不会产生事件，属已知限制。
 */
#ifndef RD91_SHOCK_DETECT_H
#define RD91_SHOCK_DETECT_H

#include <stdint.h>

/* 检测参数，由适配层从 inc/project_config.h 填入。 */
typedef struct
{
    uint16_t enter_mg;         /* 进入事件的 |a| 阈值（mg） */
    uint16_t release_mg;       /* 事件内部的回落阈值（mg），必须小于 enter */
    uint16_t end_gap_ms;       /* 低于回落阈值持续多久算事件结束 */
    uint16_t max_event_ms;     /* 事件最长持续时间，到时限强制输出 */
    uint16_t sample_period_ms; /* 采样周期（ms），用于把样本数换算成持续时间 */
} Rd91ShockParams;

/* 事件进行中的状态。 */
typedef struct
{
    uint8_t active;        /* 1 = 正在一个冲击事件内 */
    uint8_t saturated;     /* 1 = 事件内出现过任一轴触达量程上限 */
    uint16_t peak_mg;      /* 事件内 |a| 的最大值（含重力） */
    uint16_t over_samples; /* |a| >= enter 的样本数 */
    uint32_t start_ms;     /* 首个超阈值样本的时间 */
    uint32_t last_over_ms; /* 最后一个 |a| >= release 的样本时间 */
} Rd91ShockState;

/* 一次事件结束时的汇总，供业务层记录。 */
typedef struct
{
    uint16_t peak_mg;     /* 事件内 |a| 峰值（含重力，mg） */
    uint16_t duration_ms; /* 首个超阈值样本到最后一个回落样本的时间 */
    uint16_t over_samples;/* 超进入阈值的样本数 */
    uint8_t saturated;    /* 1 = 峰值已被量程截断，不能当真值 */
} Rd91ShockEvent;

void Rd91Shock_Reset(Rd91ShockState *state);

/*
 * 处理一个新样本。
 * 输入：
 *   now_ms   采样时间（毫秒）
 *   norm_mg  本样本三轴合矢量 |a|（mg）
 *   saturated 本样本是否有轴触达量程上限（1=是）
 *   params   阈值
 *   event    仅在返回 1 时填充
 * 输出：1 = 本样本结束了一个事件，event 已填充；0 = 事件未结束或无事件。
 *
 * 聚合规则：连续/带小间隔的超阈值样本算一个事件；|a| 低于 release 且持续
 * end_gap_ms 以上才算结束。事件最长 max_event_ms，到时限强制结束。
 */
int Rd91Shock_Update(Rd91ShockState *state, uint32_t now_ms, uint16_t norm_mg,
                     uint8_t saturated, const Rd91ShockParams *params,
                     Rd91ShockEvent *event);

#endif /* RD91_SHOCK_DETECT_H */
