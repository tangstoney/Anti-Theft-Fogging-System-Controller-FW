/**
 * @file impact.c
 * @brief 冲击过滤与窗口计数的纯 C 实现。
 */
#include "impact.h"

void Rd91Impact_Reset(Rd91ImpactWindow *window)
{
    window->window_start_ms = 0U;
    window->count = 0U;
}

int Rd91Impact_IsValid(uint16_t peak_g_x100, uint16_t pulse_ms,
                       uint16_t threshold_g_x100, uint16_t pulse_max_ms)
{
    if (peak_g_x100 < threshold_g_x100)
    {
        return 0;
    }
    /* 脉冲宽度必须为正且不超过上限；0ms 采样视为无效，避免除零式误判。 */
    if (pulse_ms == 0U || pulse_ms > pulse_max_ms)
    {
        return 0;
    }
    return 1;
}

uint8_t Rd91Impact_Accumulate(Rd91ImpactWindow *window, uint32_t now_ms, uint32_t window_ms)
{
    /* 无符号差值处理 tick 回绕。 */
    if (window->count == 0U || (uint32_t)(now_ms - window->window_start_ms) >= window_ms)
    {
        window->window_start_ms = now_ms;
        window->count = 0U;
    }

    if (window->count < 255U)
    {
        window->count++;
    }
    return window->count;
}
