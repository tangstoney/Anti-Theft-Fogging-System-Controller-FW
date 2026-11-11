/**
 * @file button.h
 * @brief 基于单调 tick 的按键消抖状态机。
 *
 * 只消费"原始按下/释放"电平，不直接访问 GPIO；便于在 PC 上单独审阅逻辑。
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

typedef enum
{
    BUTTON_EVENT_NONE = 0, /* 无事件 */
    BUTTON_EVENT_PRESS     /* 稳定按下（消抖完成），每次按下仅上报一次 */
} ButtonEvent;

typedef enum
{
    BUTTON_STATE_BOOT_WAIT = 0, /* 上电时按键已按住：等稳定释放 */
    BUTTON_STATE_IDLE,          /* 等待按下 */
    BUTTON_STATE_DEBOUNCE_PRESS,/* 按下抖动确认中 */
    BUTTON_STATE_PRESSED,       /* 已稳定按下 */
    BUTTON_STATE_DEBOUNCE_RELEASE /* 释放抖动确认中 */
} ButtonState;

typedef struct
{
    ButtonState state;
    uint32_t    debounce_ms;
    uint32_t    timestamp_ms;
} Button;

/* 初始化。raw_pressed 为上电瞬间的原始电平（1 = 按下）。 */
void Button_Init(Button* button, int raw_pressed, uint32_t now_ms, uint32_t debounce_ms);

/* 周期调用；返回本次需要处理的事件。 */
ButtonEvent Button_Update(Button* button, int raw_pressed, uint32_t now_ms);

#endif /* BUTTON_H */
