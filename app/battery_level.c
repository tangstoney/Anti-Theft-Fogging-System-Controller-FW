/**
 * @file battery_level.c
 * @brief 电量计原始值换算与有效性判断。
 */
#include "battery_level.h"

void Rd91Battery_Reset(Rd91BatteryState *state)
{
    state->percent = 0U;
    state->valid = 0U;
    state->last_ok_ms = 0U;
}

uint8_t Rd91Battery_SocValid(uint16_t soc_raw)
{
    return (soc_raw <= RD91_BATTERY_SOC_MAX) ? 1U : 0U;
}

uint8_t Rd91Battery_Percent(uint16_t soc_raw)
{
    /* 高字节是整数 %，低字节 1/256%；+128 做四舍五入。 */
    uint32_t percent = ((uint32_t)soc_raw + 128U) >> 8;

    if (percent > 100U)
    {
        percent = 100U;
    }
    return (uint8_t)percent;
}

uint32_t Rd91Battery_VoltageMv(uint16_t vcell_raw)
{
    /* 312.5µV = 5/16 mV，整数运算避免浮点。 */
    return ((uint32_t)vcell_raw * 5U) / 16U;
}

int16_t Rd91Battery_TempTenthsC(uint8_t temp_raw)
{
    /* temp_raw × 0.5°C − 40°C，换成 0.1°C 单位。 */
    return (int16_t)(((int16_t)temp_raw * 5) - 400);
}

uint8_t Rd91Battery_Update(Rd91BatteryState *state, const Rd91BatterySample *sample,
                           uint32_t now_ms, const Rd91BatteryParams *params)
{
    if (sample->valid != 0U)
    {
        uint8_t percent = Rd91Battery_Percent(sample->soc_raw);
        uint8_t changed = 0U;

        if ((state->valid == 0U) || (percent != state->percent))
        {
            changed = 1U;
        }
        state->percent = percent;
        state->valid = 1U;
        state->last_ok_ms = now_ms;
        return changed;
    }

    /*
     * 读取失败：stale_ms 内继续显示上次的有效值（并在 stale_ms 到期时
     * 只上报一次“转为未知”），超时后 valid=0，显示端不得把它当成当前电量。
     */
    if ((state->valid != 0U) &&
        ((uint32_t)(now_ms - state->last_ok_ms) >= params->stale_ms))
    {
        state->valid = 0U;
        return 1U;
    }
    return 0U;
}
