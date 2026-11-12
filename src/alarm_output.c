/**
 * @file alarm_output.c
 * @brief 本机声光报警服务：一个静态线程独占语音、蜂鸣器、红蓝灯三路输出。
 *
 * 所有权：
 *   - 业务线程（main）只调用 AlarmOut_SetOutput() 写期望状态；它不碰 GPIO；
 *   - 本线程是唯一访问 WT588F02 / 蜂鸣器 / 双色灯的线程；
 *   - 台架灯 PB5 仍由 main 线程按键翻转，与报警灯（PA3/PA4）不是同一盏，
 *     不存在两个线程驱动同一输出的情况。
 *
 * 收敛模型（不是命令队列）：
 *   期望状态是电平量 + 版本号。连续 START、重复 STOP、快速 start/stop 都只
 *   改写期望位图；线程每次醒来把三路输出与最新期望比较，差异才动作。
 *   因此事件位合并、队列满、旧的 START 都不会让最后的 STOP 丢失。
 *
 * 时序：
 *   - 闪烁周期与 BUSY 轮询用 RTT tick（10ms）；
 *   - 语音位时序在驱动内用 DWT 微秒延时，连续数据位期间只锁调度器，
 *     SysTick/UART 中断保持开启（见 drivers/wt588f02.c）。
 */
#include <rthw.h>
#include <rtthread.h>
#include "board.h"
#include "project_config.h"
#include "rd91_types.h"
#include "alarm_output.h"
#include "wt588f02.h"

#define ALARM_STACK_SIZE   1024
#define ALARM_WAKE_BIT     0x01U
#define ALARM_TICK_MS      (1000U / RT_TICK_PER_SECOND)

#define VOICE_BIT   ((uint8_t)(1U << (uint8_t)RD91_OUT_VOICE))
#define BUZZER_BIT  ((uint8_t)(1U << (uint8_t)RD91_OUT_BUZZER))
#define LAMP_BIT    ((uint8_t)(1U << (uint8_t)RD91_OUT_LAMP))

static struct rt_thread alarm_thread;
ALIGN(8)
static rt_uint8_t alarm_stack[ALARM_STACK_SIZE];
static struct rt_event alarm_wake;

/* 期望状态：任意线程写，声光线程读。只在关中断的短临界区里修改。 */
static volatile uint32_t request_version;
static volatile uint8_t request_mask;
static volatile uint32_t stop_requested_us;
static uint8_t time_valid;
static uint8_t service_ready;

/* 以下仅声光线程访问。 */
static uint8_t voice_state;   /* Rd91AudioState */
static uint8_t buzzer_on;
static uint8_t lamp_on;
static uint8_t lamp_blue;
static uint32_t lamp_next_ms;
static uint8_t fault_mask;
static uint32_t worst_stop_us;

/* 已发布快照与通知钩子。 */
static Rd91AlarmSnapshot snapshot;
static Rd91AlarmNotify notify_fn;

static void publish(void)
{
    Rd91AlarmSnapshot snap;
    rt_base_t level;

    snap.voice = voice_state;
    snap.buzzer = buzzer_on;
    snap.lamp = lamp_on;
    snap.fault = fault_mask;
    snap.reserved[0] = 0U;
    snap.reserved[1] = 0U;
    snap.reserved[2] = 0U;

    /* version 与 requested 必须和快照一起读，避免两者来自不同一次请求。 */
    level = rt_hw_interrupt_disable();
    snap.version = request_version;
    snap.requested = request_mask;
    snapshot = snap;
    rt_hw_interrupt_enable(level);

    if (notify_fn != RT_NULL)
    {
        notify_fn(&snap);
    }
}

static void voice_fault(const char *op, int err)
{
    fault_mask |= VOICE_BIT;
    voice_state = (uint8_t)RD91_AOUT_FAULT;
    rt_kprintf("[%s] FAULT alarm_voice op=%s err=%d\n", RD91_ROLE, op, err);
}

/*
 * 等待 BUSY 变为“播放中”（低电平）。
 * 返回 1 = 观察到播放；0 = 等待期间收到停止请求；-1 = 超时。
 * 超时只说明“没有观察到播放”，不能反证芯片没收到命令。
 */
static int wait_playback(void)
{
    uint32_t poll_ms = (uint32_t)RD91_WT_BUSY_POLL_TICKS * ALARM_TICK_MS;
    uint32_t polls = (uint32_t)RD91_WT_BUSY_TIMEOUT_MS / poll_ms;
    uint32_t i;

    if (polls == 0U)
    {
        polls = 1U;
    }
    if (Wt_BusyLevel() == 0)
    {
        return 1;
    }
    for (i = 0U; i < polls; i++)
    {
        if ((request_mask & VOICE_BIT) == 0U)
        {
            //当前没有语音播放请求
            return 0;
        }
        rt_thread_delay(RD91_WT_BUSY_POLL_TICKS);
        if (Wt_BusyLevel() == 0)
        {
            return 1;
        }
    }
    return -1;
}

static void stop_voice(void)
{
    int ret;
    uint32_t latency_us = 0U;

    ret = Wt_SendStop();

    /* 停止延迟从“期望状态被改写”的时刻算到 FE 完整发完。 */
    if ((time_valid != 0U) && (stop_requested_us != 0U))
    {
        latency_us = (uint32_t)(Wt_NowUs() - stop_requested_us);
        if (latency_us > worst_stop_us)
        {
            worst_stop_us = latency_us;
        }
        stop_requested_us = 0U;
    }

    if (ret != WT_OK)
    {
        fault_mask |= VOICE_BIT;
        rt_kprintf("[%s] FAULT alarm_voice op=stop err=%d\n", RD91_ROLE, ret);
    }
    voice_state = (uint8_t)RD91_AOUT_OFF;
    rt_kprintf("[%s] ALARM_OUT_STOP stop_us=%u worst_us=%u\n", RD91_ROLE,
               (unsigned int)latency_us, (unsigned int)worst_stop_us);
}

/* 启动语音：播放段地址 -> F2 循环 -> 用 BUSY 确认。每条命令后检查最新请求。 */
static void start_voice(void)
{
    int ret;

    voice_state = (uint8_t)RD91_AOUT_CMD_SENT;
    fault_mask &= (uint8_t)~VOICE_BIT;

    ret = Wt_SendPlay((uint16_t)RD91_WT_SEGMENT_ALARM);
    if (ret != WT_OK)
    {
        voice_fault("play", ret);
        return;
    }
    rt_kprintf("[%s] ALARM_VOICE_CMD seg=%u\n", RD91_ROLE,
               (unsigned int)RD91_WT_SEGMENT_ALARM);

    /* 一条完整命令后重新核对：期间若已撤警，不再发循环命令。 */
    if ((request_mask & VOICE_BIT) == 0U)
    {
        return;
    }

    ret = Wt_SendLoop();
    if (ret != WT_OK)
    {
        voice_fault("loop", ret);
        return;
    }

#if RD91_WT_BUSY_ENABLED
    ret = wait_playback();
    if (ret == 1)
    {
        voice_state = (uint8_t)RD91_AOUT_ON;
        rt_kprintf("[%s] ALARM_VOICE_PLAYING seg=%u\n", RD91_ROLE,
                   (unsigned int)RD91_WT_SEGMENT_ALARM);
    }
    else if (ret == 0)
    {
        /* 等待期间收到停止请求：不报故障，下一轮收敛会发 FE。 */
        rt_kprintf("[%s] ALARM_VOICE_ABORTED_BY_STOP\n", RD91_ROLE);
    }
    else
    {
        voice_fault("busy", WT_ERR_BUSY);
    }
#else
    /* BUSY 未接入：只能确认命令已发，不能声称正在播放。 */
    rt_kprintf("[%s] ALARM_VOICE_CMD_SENT busy=DISABLED\n", RD91_ROLE);
#endif
}

/* 返回 1 表示语音状态有变化，需要发布快照。 */
static uint8_t converge_voice(uint8_t want)
{
    if ((want & VOICE_BIT) != 0U)
    {
        if (voice_state == (uint8_t)RD91_AOUT_OFF)
        {
            start_voice();
            return 1U;
        }
        /* CMD_SENT/ON：重复 START 幂等，保持当前播放。
         * FAULT：不原地重试，等 STOP 后再次 START（新的 0->1）才重发。 */
        return 0U;
    }

    if (voice_state != (uint8_t)RD91_AOUT_OFF)
    {
        stop_voice();
        return 1U;
    }
    return 0U;
}

static void converge(void)
{
    uint8_t want = request_mask;
    uint8_t changed = 0U;
    uint32_t now;

    /* 蜂鸣器：先处理，不等语音。 */
    if (((want & BUZZER_BIT) != 0U) && (buzzer_on == 0U))
    {
        Board_BuzzerDrive(1);
        buzzer_on = 1U;
        changed = 1U;
        rt_kprintf("[%s] ALARM_OUT_BUZZER on=1\n", RD91_ROLE);
    }
    else if (((want & BUZZER_BIT) == 0U) && (buzzer_on != 0U))
    {
        Board_BuzzerDrive(0);
        buzzer_on = 0U;
        changed = 1U;
        rt_kprintf("[%s] ALARM_OUT_BUZZER on=0\n", RD91_ROLE);
    }

    /* 红蓝灯：开始交替 / 立即熄灭。 */
    if (((want & LAMP_BIT) != 0U) && (lamp_on == 0U))
    {
        lamp_on = 1U;
        lamp_blue = 0U;
        Board_LampDrive(1, 0);
        lamp_next_ms = Bench_NowMs() + RD91_ALARM_LAMP_TOGGLE_MS;
        changed = 1U;
        rt_kprintf("[%s] ALARM_OUT_LAMP blink=1 period_ms=%u\n", RD91_ROLE,
                   (unsigned int)RD91_ALARM_LAMP_TOGGLE_MS);
    }
    else if (((want & LAMP_BIT) == 0U) && (lamp_on != 0U))
    {
        Board_LampDrive(0, 0);
        lamp_on = 0U;
        changed = 1U;
        rt_kprintf("[%s] ALARM_OUT_LAMP blink=0\n", RD91_ROLE);
    }
    else if (lamp_on != 0U)
    {
        now = Bench_NowMs();
        if ((int32_t)(now - lamp_next_ms) >= 0)
        {
            lamp_blue ^= 1U;
            Board_LampDrive((lamp_blue == 0U) ? 1 : 0, (lamp_blue != 0U) ? 1 : 0);
            lamp_next_ms += RD91_ALARM_LAMP_TOGGLE_MS;
            if ((int32_t)(now - lamp_next_ms) >= 0)
            {
                /* 线程被延迟超过一个周期时不要连续补闪，直接对齐到当前时刻。 */
                lamp_next_ms = now + RD91_ALARM_LAMP_TOGGLE_MS;
            }
        }
    }

    /* 语音放在最后：启动时要发多条带微秒时序的命令，可能占用几十毫秒。 */
    changed |= converge_voice(want);

    if (changed != 0U)
    {
        publish();
    }
}

static void alarm_entry(void *parameter)
{
    int ret;

    (void)parameter;

    ret = Wt_Init();
    if (ret != WT_OK)
    {
        fault_mask |= VOICE_BIT;
        rt_kprintf("[%s] FAULT wt_init=%d\n", RD91_ROLE, ret);
    }
    else
    {
        rt_kprintf("[%s] WT_READY wire=2WIRE addr_mode=%s seg=%u busy=%u\n", RD91_ROLE,
                   (RD91_WT_TWO_BYTE_ADDR != 0U) ? "DOUBLE" : "SINGLE",
                   (unsigned int)RD91_WT_SEGMENT_ALARM,
                   (unsigned int)RD91_WT_BUSY_ENABLED);
    }
    publish();

    for (;;)
    {
        rt_int32_t timeout = RT_WAITING_FOREVER;
        rt_uint32_t recved = 0U;
        rt_err_t result;

        if (lamp_on != 0U)
        {
            int32_t remain = (int32_t)(lamp_next_ms - Bench_NowMs());
            if (remain <= 0)
            {
                timeout = 1;
            }
            else
            {
                timeout = (rt_int32_t)(((uint32_t)remain + (ALARM_TICK_MS - 1U)) / ALARM_TICK_MS);
            }
        }

        /* 事件位只是“有变化了”的合并通知；真正的状态在 request_mask/version。 */
        result = rt_event_recv(&alarm_wake, ALARM_WAKE_BIT,
                               RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &recved);

        /*
         * 业务层按 VOICE/BUZZER/LAMP 逐个回调，三次回调发生在同一瞬间。
         * 被事件唤醒后先让出 1 个 tick，让调用方把三路都写完再统一收敛，
         * 避免声光线程按“只有语音”这一中间状态先发码、把业务线程挡住
         * 几十毫秒。代价是所有期望状态变化最多晚 10ms 生效，停止延迟
         * 仍然实测并记录在 ALARM_OUT_STOP 日志里。
         */
        if ((result == RT_EOK) && (recved != 0U))
        {
            rt_thread_delay(1);
        }

        converge();
    }
}

void AlarmOut_Init(void)
{
    rt_err_t ret;

    /* 先把三路输出置为关闭态，再启动线程。 */
    Board_AlarmIoInit();
    service_ready = 0U;
    voice_state = (uint8_t)RD91_AOUT_OFF;
    buzzer_on = 0U;
    lamp_on = 0U;
    lamp_blue = 0U;
    fault_mask = 0U;
    request_mask = 0U;
    request_version = 0U;
    stop_requested_us = 0U;
    worst_stop_us = 0U;

    notify_fn = RT_NULL;
    snapshot.version = 0U;
    snapshot.requested = 0U;
    snapshot.voice = (uint8_t)RD91_AOUT_OFF;
    snapshot.buzzer = 0U;
    snapshot.lamp = 0U;
    snapshot.fault = 0U;
    snapshot.reserved[0] = 0U;
    snapshot.reserved[1] = 0U;
    snapshot.reserved[2] = 0U;

    time_valid = (Wt_ClockInit() == WT_OK) ? 1U : 0U;
    if (time_valid == 0U)
    {
        fault_mask |= VOICE_BIT;
        rt_kprintf("[%s] FAULT wt_dwt\n", RD91_ROLE);
    }

    ret = rt_event_init(&alarm_wake, "alarm", RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK)
    {
        rt_kprintf("[%s] FAULT alarm_event=%d\n", RD91_ROLE, ret);
        return;
    }
    service_ready = 1U;

    ret = rt_thread_init(&alarm_thread, "alarm", alarm_entry, RT_NULL,
                         alarm_stack, sizeof(alarm_stack), 3, 5);
    if (ret != RT_EOK)
    {
        rt_kprintf("[%s] FAULT alarm_thread=%d\n", RD91_ROLE, ret);
        return;
    }

    ret = rt_thread_startup(&alarm_thread);
    if (ret != RT_EOK)
    {
        rt_kprintf("[%s] FAULT alarm_thread_start=%d\n", RD91_ROLE, ret);
    }
}

void AlarmOut_SetOutput(uint8_t output, uint8_t on)
{
    rt_base_t level;
    uint8_t bit;
    uint8_t new_mask;
    uint8_t wake = 0U;

    if (output >= (uint8_t)RD91_OUT_COUNT)
    {
        return;
    }

    bit = (uint8_t)(1U << output);

    level = rt_hw_interrupt_disable();
    new_mask = request_mask;
    if (on != 0U)
    {
        new_mask |= bit;
    }
    else
    {
        new_mask &= (uint8_t)~bit;
    }

    if (new_mask != request_mask)
    {
        /* 记录语音停止请求的时刻，用于统计“请求 -> FE 发完”的最坏延迟；
         * 重新启动时清掉旧时间戳，避免延迟统计跨请求。 */
        if (output == (uint8_t)RD91_OUT_VOICE)
        {
            if (((request_mask & VOICE_BIT) != 0U) && (on == 0U) && (time_valid != 0U))
            {
                stop_requested_us = Wt_NowUs();
                if (stop_requested_us == 0U)
                {
                    /* 0 在 DWT 回绕点可能出现；用非 0 值表示“有请求”。 */
                    stop_requested_us = 1U;
                }
            }
            else if (((request_mask & VOICE_BIT) == 0U) && (on != 0U))
            {
                stop_requested_us = 0U;
            }
        }
        request_mask = new_mask;
        request_version++;
        wake = 1U;
    }
    rt_hw_interrupt_enable(level);

    if (wake != 0U)
    {
        /* 初始化失败时只保留期望状态，不能对未初始化的 IPC 对象发事件。 */
        if (service_ready != 0U)
        {
            rt_event_send(&alarm_wake, ALARM_WAKE_BIT);
        }
    }
}

void AlarmOut_GetSnapshot(Rd91AlarmSnapshot *out)
{
    rt_base_t level;

    if (out == RT_NULL)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    *out = snapshot;
    rt_hw_interrupt_enable(level);
}

void AlarmOut_SetNotify(Rd91AlarmNotify notify)
{
    notify_fn = notify;
}
