/**
 * @file shock_detect.c
 * @brief 疑似冲击链路：逐样本峰值检测与事件聚合。
 */
#include "shock_detect.h"

void Rd91Shock_Reset(Rd91ShockState *state)
{
    state->active = 0U;
    state->saturated = 0U;
    state->peak_mg = 0U;
    state->over_samples = 0U;
    state->start_ms = 0U;
    state->last_over_ms = 0U;
}

static void shock_close(Rd91ShockState *state, const Rd91ShockParams *params,
                        Rd91ShockEvent *event)
{
    /* 持续时间 = 首个超阈值样本到最后一个达到回落阈值的样本，再加上这一样本
     * 自身的采样周期。采样周期是 10ms，所以单个样本的事件应报告 10ms，
     * 而不是 1ms；这也是本链路的时间分辨率下限。 */
    uint32_t span = (uint32_t)(state->last_over_ms - state->start_ms);

    event->peak_mg = state->peak_mg;
    event->duration_ms = (uint16_t)(span + params->sample_period_ms);
    event->over_samples = state->over_samples;
    event->saturated = state->saturated;

    Rd91Shock_Reset(state);
}

int Rd91Shock_Update(Rd91ShockState *state, uint32_t now_ms, uint16_t norm_mg,
                     uint8_t saturated, const Rd91ShockParams *params,
                     Rd91ShockEvent *event)
{
    if (state->active == 0U)
    {
        if (norm_mg >= params->enter_mg)
        {
            state->active = 1U;
            state->saturated = (saturated != 0U) ? 1U : 0U;
            state->peak_mg = norm_mg;
            state->over_samples = 1U;
            state->start_ms = now_ms;
            state->last_over_ms = now_ms;
        }
        return 0;
    }

    /* 事件进行中：先更新峰值和饱和标志，再判断是否结束。 */
    if (norm_mg > state->peak_mg)
    {
        state->peak_mg = norm_mg;
    }
    if (saturated != 0U)
    {
        state->saturated = 1U;
    }

    if (norm_mg >= params->release_mg)
    {
        state->last_over_ms = now_ms;
        if (norm_mg >= params->enter_mg && state->over_samples != 0xFFFFU)
        {
            state->over_samples++;
        }
    }
    else if ((uint32_t)(now_ms - state->last_over_ms) >= params->end_gap_ms)
    {
        shock_close(state, params, event);
        return 1;
    }

    /* 持续剧烈振动时防止事件无限延长：到时限强制输出。 */
    if ((uint32_t)(now_ms - state->start_ms) >= params->max_event_ms)
    {
        shock_close(state, params, event);
        return 1;
    }

    return 0;
}
