/**
 * @file impact.h
 * @brief 冲击事件的有效性过滤与窗口计数（纯 C，可在主机侧单测）。
 *
 * 只做"单次采样是否像冲击"和"窗口内累计次数"，不判断砸车/拖车语义。
 * 阈值来源：P1 §6.2 给出高 g、脉冲 <150ms、窗口内多次确认，具体数值待标定。
 */
#ifndef RD91_IMPACT_H
#define RD91_IMPACT_H

#include <stdint.h>

typedef struct
{
    uint32_t window_start_ms;
    uint8_t count;
} Rd91ImpactWindow;

void Rd91Impact_Reset(Rd91ImpactWindow *window);

/* 1 = 满足高 g 且脉冲足够短的有效冲击采样。 */
int Rd91Impact_IsValid(uint16_t peak_g_x100, uint16_t pulse_ms,
                       uint16_t threshold_g_x100, uint16_t pulse_max_ms);

/* 计入一次有效冲击；窗口过期则重新开始。返回当前窗口内的次数。 */
uint8_t Rd91Impact_Accumulate(Rd91ImpactWindow *window, uint32_t now_ms, uint32_t window_ms);

#endif /* RD91_IMPACT_H */
