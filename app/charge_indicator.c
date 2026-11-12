/**
 * @file charge_indicator.c
 * @brief 充电/状态指示灯判定：寄存器状态解码、去抖与灯色计算。
 */
#include "charge_indicator.h"

/* NTC_FAULT 编码（REG09 D[2:0]）：101=冷、110=热时充电被挂起。 */
#define NTC_COLD 5U
#define NTC_HOT  6U

void Rd91Charge_Reset(Rd91ChargeJudge *judge)
{
    judge->state = (uint8_t)RD91_CHG_UNKNOWN;
    judge->candidate = (uint8_t)RD91_CHG_UNKNOWN;
    judge->candidate_since = 0U;
}

static uint8_t classify(const Rd91ChargeSample *sample)
{
    if (sample->valid == 0U)
    {
        return (uint8_t)RD91_CHG_UNKNOWN;
    }

    /* 没有可用输入时先判“无输入”：此时残留的故障位不应让灯闪。 */
    if (sample->pg_stat == 0U)
    {
        return (uint8_t)RD91_CHG_NO_INPUT;
    }

    if ((sample->chrg_fault != 0U) || (sample->bat_fault != 0U) ||
        (sample->ntc_fault == NTC_COLD) || (sample->ntc_fault == NTC_HOT))
    {
        return (uint8_t)RD91_CHG_FAULT;
    }

    switch (sample->chrg_stat)
    {
        case 1U: /* 预充 */
        case 2U: /* 快充（恒流/恒压） */
            return (uint8_t)RD91_CHG_CHARGING;
        case 3U: /* 充电完成 */
            return (uint8_t)RD91_CHG_FULL;
        default: /* 0 = 充电被禁用 */
            return (uint8_t)RD91_CHG_DISABLED;
    }
}

uint8_t Rd91Charge_Update(Rd91ChargeJudge *judge, const Rd91ChargeSample *sample,
                          uint32_t now_ms, const Rd91ChargeParams *params)
{
    uint8_t raw = classify(sample);
    uint8_t changed = 0U;

    /* 去抖：原始状态保持 debounce_ms 不变才切换，插拔/故障瞬间不会来回闪。 */
    if (raw != judge->candidate)
    {
        judge->candidate = raw;
        judge->candidate_since = now_ms;
    }
    else if ((uint32_t)(now_ms - judge->candidate_since) >= params->debounce_ms)
    {
        if (judge->state != judge->candidate)
        {
            judge->state = judge->candidate;
            changed = 1U;
        }
    }

    return changed;
}

void Rd91Charge_LedLevels(uint8_t state, const Rd91ChargeParams *params,
                          uint32_t now_ms, uint8_t *red_on, uint8_t *green_on)
{
    *red_on = 0U;
    *green_on = 0U;

    switch (state)
    {
        case (uint8_t)RD91_CHG_CHARGING:
            *red_on = 1U;
            break;
        case (uint8_t)RD91_CHG_FULL:
            *green_on = 1U;
            break;
        case (uint8_t)RD91_CHG_FAULT:
            /* 亮/灭各 blink_ms，用绝对时间分块；uint32 回绕只造成一次相位跳变。 */
            if (((now_ms / params->fault_blink_ms) & 1U) == 0U)
            {
                *red_on = 1U;
            }
            break;
        default:
            /* NO_INPUT / DISABLED / UNKNOWN：熄灭。 */
            break;
    }
}
