/**
 * @file motion_stats.c
 * @brief 运动状态链路：窗口累加、RMS 计算与迟滞判断。
 */
#include "motion_stats.h"
#include "rd91_math.h"

void Rd91Motion_AccumReset(Rd91MotionAccum *accum)
{
    accum->sum[0] = 0;
    accum->sum[1] = 0;
    accum->sum[2] = 0;
    accum->sumsq = 0;
    accum->samples = 0U;
}

void Rd91Motion_AccumAdd(Rd91MotionAccum *accum, int32_t x_mg, int32_t y_mg, int32_t z_mg)
{
    int64_t x = (int64_t)x_mg;
    int64_t y = (int64_t)y_mg;
    int64_t z = (int64_t)z_mg;

    accum->sum[0] += x;
    accum->sum[1] += y;
    accum->sum[2] += z;
    accum->sumsq += x * x + y * y + z * z;

    if (accum->samples != 0xFFFFFFFFU)
    {
        accum->samples++;
    }
}

uint32_t Rd91Motion_AccumRmsMg(const Rd91MotionAccum *accum)
{
    int64_t n;
    int64_t mean_sq;
    int64_t var_sum;

    if (accum->samples == 0U)
    {
        return 0U;
    }

    n = (int64_t)accum->samples;

    /*
     * 用恒等式避免第二遍遍历（不保存样本）：
     *   Σ(x-m)^2 = Σx^2 - (Σx)^2 / n
     * 三轴相加后除以 n，再开方就是题目里的 RMS。
     * 中间量：单轴 |Σx| <= 8000mg × 100 样本 = 8e5，平方后 6.4e11，
     * int64 足够；不会溢出。
     */
    mean_sq = (accum->sum[0] * accum->sum[0] +
               accum->sum[1] * accum->sum[1] +
               accum->sum[2] * accum->sum[2]) / n;

    var_sum = accum->sumsq - mean_sq;
    if (var_sum < 0)
    {
        /* 整数除法截断可能让差值略小于 0，按 0 处理。 */
        return 0U;
    }

    return Rd91_IsqrtU64((uint64_t)var_sum / (uint64_t)n);
}

void Rd91Motion_JudgeReset(Rd91MotionJudge *judge, uint8_t initial_state)
{
    judge->state = initial_state;
    judge->enter_count = 0U;
    judge->exit_count = 0U;
}

uint8_t Rd91Motion_Judge(Rd91MotionJudge *judge, uint32_t rms_mg,
                         uint32_t valid_samples, uint32_t errors,
                         const Rd91MotionParams *params)
{
    /* 故障优先：不能把读不到的数据累计成“停车”。 */
    if (errors >= (uint32_t)params->fault_errors)
    {
        judge->enter_count = 0U;
        judge->exit_count = 0U;
        judge->state = (uint8_t)RD91_VEHICLE_FAULT;
        return judge->state;
    }

    /* 窗口样本太少同样不可信，判 UNKNOWN 而不是 STILL。 */
    if (valid_samples < params->min_valid_samples)
    {
        judge->enter_count = 0U;
        judge->exit_count = 0U;
        judge->state = (uint8_t)RD91_VEHICLE_UNKNOWN;
        return judge->state;
    }

    if (rms_mg >= (uint32_t)params->enter_rms_mg)
    {
        judge->exit_count = 0U;
        if (judge->enter_count != 0xFFU)
        {
            judge->enter_count++;
        }
        if (judge->enter_count >= params->enter_windows)
        {
            judge->enter_count = 0U;
            judge->state = (uint8_t)RD91_VEHICLE_MOVING;
        }
    }
    else if (rms_mg <= (uint32_t)params->exit_rms_mg)
    {
        judge->enter_count = 0U;
        if (judge->exit_count != 0xFFU)
        {
            judge->exit_count++;
        }
        if (judge->exit_count >= params->exit_windows)
        {
            judge->exit_count = 0U;
            judge->state = (uint8_t)RD91_VEHICLE_STILL;
        }
    }
    else
    {
        /* 落在迟滞带内：两个方向都不算数，保持在原状态。 */
        judge->enter_count = 0U;
        judge->exit_count = 0U;
    }

    return judge->state;
}
