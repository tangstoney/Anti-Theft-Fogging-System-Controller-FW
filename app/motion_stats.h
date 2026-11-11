/**
 * @file motion_stats.h
 * @brief 运动状态链路的纯 C 实现：秒级窗口振动 RMS + 迟滞判断。
 *
 * 输入是逐个新样本（单位 mg），不是秒级平均值；
 * 输出是 UNKNOWN / STILL / MOVING 三态之一。
 *
 * 指标说明（为什么用“去均值三轴 RMS”）：
 *   rms = sqrt(mean((ax-mean_ax)^2 + (ay-mean_ay)^2 + (az-mean_az)^2))
 * 停车时三轴读数基本恒定（重力方向固定），减去各自均值后只剩很小的噪声，
 * rms 很小；行驶时车身持续不规则振动，rms 明显变大。
 * 去均值同时把“停在坡上”的重力分量扣掉，坡度不会让车被判成行驶。
 *
 * 这不是测速，也不是姿态解算；只是“车辆在振动还是在静止”的推断。
 */
#ifndef RD91_MOTION_STATS_H
#define RD91_MOTION_STATS_H

#include <stdint.h>
#include "rd91_types.h"

/* 一个统计窗口内的累加器。所有量单位为 mg 或 mg^2。 */
typedef struct
{
    int64_t sum[3];   /* Σax、Σay、Σaz */
    int64_t sumsq;    /* Σ(ax^2 + ay^2 + az^2) */
    uint32_t samples; /* 参与累加的有效样本数 */
} Rd91MotionAccum;

/* 判断参数。由适配层从 inc/project_config.h 填入，模块本身不含阈值。 */
typedef struct
{
    uint32_t min_valid_samples; /* 窗口内最少有效样本数 */
    uint16_t enter_rms_mg;      /* 进入“行驶”的 RMS 阈值 */
    uint16_t exit_rms_mg;       /* 回到“停车”的 RMS 阈值 */
    uint8_t enter_windows;      /* 连续多少窗口满足才进入行驶 */
    uint8_t exit_windows;       /* 连续多少窗口满足才回到停车 */
    uint8_t fault_errors;       /* 窗口内错误数达到多少判 FAULT */
} Rd91MotionParams;

/* 窗口之间的确认计数与当前状态。 */
typedef struct
{
    uint8_t state;       /* Rd91VehicleState */
    uint8_t enter_count; /* 连续满足“进入行驶”条件的窗口数 */
    uint8_t exit_count;  /* 连续满足“回到停车”条件的窗口数 */
} Rd91MotionJudge;

void Rd91Motion_AccumReset(Rd91MotionAccum *accum);

/* 加入一个样本。调用者负责换算成 mg，并决定是否排除冲击事件期间的数据。 */
void Rd91Motion_AccumAdd(Rd91MotionAccum *accum, int32_t x_mg, int32_t y_mg, int32_t z_mg);

/* 计算去均值三轴 RMS（mg）。样本数为 0 时返回 0。 */
uint32_t Rd91Motion_AccumRmsMg(const Rd91MotionAccum *accum);

/* 初始化判断器。initial_state 通常为 RD91_VEHICLE_UNKNOWN。 */
void Rd91Motion_JudgeReset(Rd91MotionJudge *judge, uint8_t initial_state);

/*
 * 结束一个窗口并判断状态。
 * 输入：
 *   rms_mg        本窗口去均值 RMS（仅在样本数足够时有意义）
 *   valid_samples 本窗口有效样本数
 *   errors        本窗口 I2C 错误次数
 * 输出：判断后的车辆状态（UNKNOWN/STILL/MOVING/FAULT）。
 *
 * 规则：
 *   - 错误数达到 fault_errors       → FAULT，并清零确认计数；
 *   - 样本数不足 min_valid_samples  → UNKNOWN，并清零确认计数；
 *   - 否则按两个阈值做迟滞：连续 enter_windows 个窗口 rms >= enter 判 MOVING，
 *     连续 exit_windows 个窗口 rms <= exit 判 STILL，落在中间带则计数清零。
 * UNKNOWN/FAULT 也走同一套确认逻辑，所以传感器恢复后能重新落到 STILL/MOVING。
 */
uint8_t Rd91Motion_Judge(Rd91MotionJudge *judge, uint32_t rms_mg,
                         uint32_t valid_samples, uint32_t errors,
                         const Rd91MotionParams *params);

#endif /* RD91_MOTION_STATS_H */
