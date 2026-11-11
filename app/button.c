/**
 * @file button.c
 * @brief 基于单调 tick 的按键消抖状态机。
 */
#include "button.h"

void Button_Init(Button* button, int raw_pressed, uint32_t now_ms, uint32_t debounce_ms)
{
    button->debounce_ms  = debounce_ms;
    button->timestamp_ms = now_ms;

    /* 上电时按键已按住：先等稳定释放，避免把按住状态算成一次按下。 */
    button->state = (raw_pressed != 0) ? BUTTON_STATE_BOOT_WAIT : BUTTON_STATE_IDLE;
}

ButtonEvent Button_Update(Button* button, int raw_pressed, uint32_t now_ms)
{
    int elapsed_enough = ((uint32_t)(now_ms - button->timestamp_ms) >= button->debounce_ms);

    switch (button->state)
    {
        case BUTTON_STATE_BOOT_WAIT:
            if (raw_pressed != 0)
            {
                /* 仍按住（或又按下）：重新计时，直到连续稳定释放。 */
                button->timestamp_ms = now_ms;
            }
            else if (elapsed_enough)
            {
                button->state = BUTTON_STATE_IDLE;
            }
            break;

        case BUTTON_STATE_IDLE:
            if (raw_pressed != 0)
            {
                button->state        = BUTTON_STATE_DEBOUNCE_PRESS;
                button->timestamp_ms = now_ms;
            }
            break;

        case BUTTON_STATE_DEBOUNCE_PRESS:
            if (raw_pressed == 0)
            {
                /* 抖动：回到等待状态。 */
                button->state = BUTTON_STATE_IDLE;
            }
            else if (elapsed_enough)
            {
                button->state = BUTTON_STATE_PRESSED;
                return BUTTON_EVENT_PRESS;
            }
            break;

        case BUTTON_STATE_PRESSED:
            if (raw_pressed == 0)
            {
                button->state        = BUTTON_STATE_DEBOUNCE_RELEASE;
                button->timestamp_ms = now_ms;
            }
            break;

        case BUTTON_STATE_DEBOUNCE_RELEASE:
            if (raw_pressed != 0)
            {
                /* 释放过程中的抖动：仍视为按住。 */
                button->state = BUTTON_STATE_PRESSED;
            }
            else if (elapsed_enough)
            {
                /* 稳定释放完成，允许下一次按下。 */
                button->state = BUTTON_STATE_IDLE;
            }
            break;

        default:
            button->state = BUTTON_STATE_IDLE;
            break;
    }

    return BUTTON_EVENT_NONE;
}
