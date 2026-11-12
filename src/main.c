/**
 * @file main.c
 * @brief RTT 适配层：线程、消息队列、板级 I/O 与业务状态机的连接。
 *
 * 线程划分：
 *   - main  ：唯一修改业务状态的线程，消费事件队列；
 *   - input ：按键消抖，只投递事件；
 *   - imu   ：100Hz 采样，唯一访问 IMU 寄存器的线程，只投递事件。
 *
 * 传感器数据路径（每个新样本同时喂给两条链路）：
 *   Imu_ReadFrame()
 *     ├─ 疑似冲击：Rd91Shock_Update()   逐样本，10ms 时间分辨率
 *     └─ 运动状态：Rd91Motion_AccumAdd() 累加进 1 秒窗口，窗口结束再判断
 * 两条链路使用同一份样本，但参数和判断函数完全独立。
 */
#include <rtthread.h>
#include "rd91_app.h"
#include "board.h"
#include "button.h"
#include "i2c1.h"
#include "imu_qmi8a01z.h"
#include "motion_stats.h"
#include "shock_detect.h"
#include "rd91_math.h"
#include "project_config.h"
#include "alarm_output.h"
#include "charge_led.h"
#include "battery_gauge.h"

#define EVENT_QUEUE_SLOTS 16U

/* 一个统计窗口理论上应收到的芯片样本数（ODR × 窗口秒数）。
 * 只用于日志对照：chip_n 明显小于它，说明芯片实际速率不等于配置的 ODR，
 * 或者读取被阻塞太久。1000Hz × 1s = 1000。 */
#define IMU_CHIP_SAMPLES_PER_WINDOW ((RD91_IMU_ODR_HZ * RD91_MOTION_WINDOW_MS) / 1000U)

/* 软件采样周期（ms），由 tick 频率和每周期 tick 数推导。
 * 用于把样本数换算成时间量（冲击持续时间下限）。 */
#define IMU_SAMPLE_PERIOD_MS (1000U / (RT_TICK_PER_SECOND / RD91_IMU_SAMPLE_PERIOD_TICKS))

static struct rt_messagequeue events;
/* RTT 消息队列每槽额外保存一个链表指针。 */
ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t event_pool[EVENT_QUEUE_SLOTS * (sizeof(Rd91Event) + sizeof(void *))];
static struct rt_thread input_thread;
ALIGN(8)
static rt_uint8_t input_stack[768];
static volatile uint32_t queue_drops;

/* ---------- 传感器采样线程 ---------- */

static struct rt_thread imu_thread;
ALIGN(8)
static rt_uint8_t imu_stack[1024];

static Rd91MotionAccum motion_acc;
static Rd91MotionJudge motion_judge;
static Rd91ShockState shock_state;

/* 判断参数集中在 inc/project_config.h；这里只做一次搬运。 */
static const Rd91MotionParams motion_params =
{
    RD91_MOTION_MIN_VALID_SAMPLES,
    RD91_MOTION_ENTER_RMS_MG,
    RD91_MOTION_EXIT_RMS_MG,
    RD91_MOTION_ENTER_WINDOWS,
    RD91_MOTION_EXIT_WINDOWS,
    RD91_MOTION_FAULT_ERRORS
};

static const Rd91ShockParams shock_params =
{
    RD91_SHOCK_ENTER_MG,
    RD91_SHOCK_RELEASE_MG,
    RD91_SHOCK_END_GAP_MS,
    RD91_SHOCK_MAX_EVENT_MS,
    IMU_SAMPLE_PERIOD_MS
};

/* 返回 0 = 已进入队列；非 0 = 队列满，事件丢失。 */
static int post_event(const Rd91Event *event)
{
    if (rt_mq_send(&events, event, sizeof(*event)) != RT_EOK)
    {
        queue_drops++;
        return -1;
    }
    return 0;
}

/*
 * 按键线程只管按键；IMU 初始化放在 main 线程里做，
 * 因为初始化要连续做几十毫秒的 I2C 事务，放在 768 字节栈的 input 线程里不合适。
 */
static int imu_bringup(void)
{
    int ret;

    if (I2c1_Init() != I2C1_OK)
    {
        rt_kprintf("[%s] FAULT i2c1_init\n", RD91_ROLE);
        return -1;
    }

    /* 先配引脚和 EXTI，再初始化芯片；顺序反了可能丢掉最早的边沿。 */
    Board_ImuIntInit();

    ret = Imu_Init();
    if (ret == IMU_OK)
    {
        rt_kprintf("[%s] IMU_OK addr=0x%02X\n", RD91_ROLE,
                   (unsigned int)RD91_IMU_I2C_ADDR7);
    }
    else
    {
        /* 模块没接好也要能继续跑：其余自检功能不受影响。 */
        rt_kprintf("[%s] FAULT imu_init=%d\n", RD91_ROLE, ret);
    }
    return ret;
}

/* ---------- 输出桩 ---------- */

static void log_sink(const char *event, int32_t v1, int32_t v2)
{
    rt_kprintf("[%s] t=%u %s v1=%d v2=%d\n", RD91_ROLE,
               (unsigned int)Bench_NowMs(), event, (int)v1, (int)v2);
}

static void keyfob_reply_sink(uint32_t request_id, uint32_t session,
                              uint8_t result, uint8_t arm_state)
{
    /* 业务回执已生成；BLE/AT 传输尚未实现，不得当作遥控器已收到。 */
    rt_kprintf("[%s] KEYFOB_REPLY_STUB rid=%u session=%u result=%u arm=%u\n",
               RD91_ROLE, (unsigned int)request_id, (unsigned int)session,
               (unsigned int)result, (unsigned int)arm_state);
}

static void alarm_output_sink(uint8_t output, uint8_t on)
{
    /*
     * 业务层只登记“期望状态”，立即返回：真正的语音时序、蜂鸣器、灯
     * 由声光线程按最新期望收敛。这里不点台架灯——报警灯与台架灯是两套
     * 输出，避免两个线程驱动同一盏灯。
     * 连续 START / 重复 STOP 由 AlarmOut_SetOutput 做幂等处理。
     */
    AlarmOut_SetOutput(output, on);
}

static void link_action_sink(uint8_t peer, uint8_t action, int32_t arg)
{
    rt_kprintf("[%s] LINK_ACTION_STUB peer=%u action=%u arg=%d\n",
               RD91_ROLE, (unsigned int)peer, (unsigned int)action, (int)arg);
}

/*
 * 电量事件里的“是否充电”来自充电器状态，而不是电量计：
 * 充电中=1、已充满/无输入/充电禁用=0、状态未知或故障=0xFF（未知）。
 * 不能用 0 冒充“未充电”——未被确认的事实不能当作有效数据。
 */
static uint8_t battery_charging_flag(void)
{
    switch (ChargeLed_GetState())
    {
        case (uint8_t)RD91_CHG_CHARGING:
            return 1U;
        case (uint8_t)RD91_CHG_FULL:
        case (uint8_t)RD91_CHG_NO_INPUT:
        case (uint8_t)RD91_CHG_DISABLED:
            return 0U;
        default:
            return RD91_BATT_CHARGING_UNKNOWN;
    }
}

static void display_sink(const Rd91Status *status)
{
    rt_kprintf("[%s] STATUS arm=%u veh=%u alarm=%u fog=%u exec=%u "
               "kf=%u fg=%u link=%u/%u/%u batt_local=%u/%u batt=%u/%u\n",
               RD91_ROLE,
               (unsigned int)status->arm, (unsigned int)status->vehicle,
               (unsigned int)status->alarm, (unsigned int)status->fog,
               (unsigned int)status->fog_exec_kind,
               (unsigned int)status->keyfob_paired, (unsigned int)status->fogger_paired,
               (unsigned int)status->link[RD91_PEER_KEYFOB],
               (unsigned int)status->link[RD91_PEER_FOGGER],
               (unsigned int)status->link[RD91_PEER_PHONE],
               (unsigned int)status->battery[RD91_BATTERY_LOCAL].percent,
               (unsigned int)status->battery[RD91_BATTERY_LOCAL].percent_valid,
               (unsigned int)status->battery[RD91_BATTERY_KEYFOB].percent,
               (unsigned int)status->battery[RD91_BATTERY_KEYFOB].percent_valid);
}

static Rd91Result fog_send_sink(uint32_t request_id, uint8_t action, uint32_t at_ms)
{
    Rd91Event event;

    /* 台架回环：仅模拟传输/执行端，结果标记 SIMULATED，绝不宣称真实喷烟。 */
    rt_kprintf("[%s] FOG_TX_STUB rid=%u action=%u\n", RD91_ROLE,
               (unsigned int)request_id, (unsigned int)action);

    event.type = (uint8_t)RD91_EV_FOG_ACK;
    event.at_ms = at_ms;
    event.request_id = request_id;
    event.u.fog.peer = (uint8_t)RD91_PEER_FOGGER;
    event.u.fog.stage = (uint8_t)RD91_FOG_STAGE_ACCEPTED;
    event.u.fog.result = (uint8_t)RD91_RESULT_ACCEPTED;
    event.u.fog.exec_kind = (uint8_t)RD91_EXEC_NONE;
    event.u.fog.detail = 0U;
    post_event(&event);

    if (action == (uint8_t)RD91_FOG_ACTION_START)
    {
        event.type = (uint8_t)RD91_EV_FOG_RESULT;
        event.u.fog.exec_kind = (uint8_t)RD91_EXEC_SIMULATED;
        event.u.fog.detail = 1U;
        post_event(&event);
    }

    /* 返回 OK 只表示传输层接受，不是业务接受，更不是喷烟成功。 */
    return RD91_RESULT_OK;
}

/* ---------- 事件投递小工具 ---------- */

static int post_vehicle_state(uint8_t state, uint32_t now_ms)
{
    Rd91Event event;

    event.type = (uint8_t)RD91_EV_VEHICLE_STATE;
    event.at_ms = now_ms;
    event.request_id = 0U;
    event.u.vehicle.state = state;
    event.u.vehicle.reserved = 0U;
    return post_event(&event);
}

static void post_wake_event(uint8_t any_motion, uint8_t status1, uint32_t now_ms)
{
    Rd91Event event;

    event.type = (uint8_t)RD91_EV_MOTION;
    event.at_ms = now_ms;
    event.request_id = 0U;
    event.u.motion.any_motion = any_motion;
    event.u.motion.status1_raw = status1;
    post_event(&event);
}

static void post_shock_event(const Rd91ShockEvent *shock, uint32_t now_ms)
{
    Rd91Event event;

    event.type = (uint8_t)RD91_EV_SHOCK;
    event.at_ms = now_ms;
    event.request_id = 0U;
    event.u.shock.peak_mg = shock->peak_mg;
    event.u.shock.duration_ms = shock->duration_ms;
    event.u.shock.over_samples = shock->over_samples;
    event.u.shock.saturated = shock->saturated;
    post_event(&event);
}

static void post_stats_event(uint32_t rms_mg, uint32_t samples, uint32_t chip_delta,
                             uint32_t dropped, uint32_t errors, uint32_t edges,
                             uint8_t state, uint32_t now_ms)
{
    Rd91Event event;

    event.type = (uint8_t)RD91_EV_IMU_STATS;
    event.at_ms = now_ms;
    event.request_id = 0U;
    event.u.stats.valid_samples = (uint16_t)samples;
    event.u.stats.rms_mg = (uint16_t)rms_mg;
    event.u.stats.chip_delta = (uint16_t)chip_delta;
    event.u.stats.dropped = (uint16_t)dropped;
    event.u.stats.int_edges = edges;
    event.u.stats.enter_count = motion_judge.enter_count;
    event.u.stats.exit_count = motion_judge.exit_count;
    event.u.stats.state = state;
    event.u.stats.errors = (uint8_t)errors;
    post_event(&event);
}

/* ---------- 采样线程 ---------- */

/*
 * 100Hz 采样。每隔一个 tick（10ms）读一帧完整样本，然后：
 *   1) 用 aDA 判断是不是新样本；旧样本不喂给任何链路，避免重复统计；
 *   2) 冲击链路逐样本处理；事件结束时投递 RD91_EV_SHOCK；
 *   3) 运动链路把样本累加进 1 秒窗口；窗口结束时判断一次并投递
 *      RD91_EV_VEHICLE_STATE（只在状态变化时）；
 *   4) 被冲击事件覆盖的样本不计入运动统计，避免一次撞击把“停车”翻成“行驶”。
 *
 * 所有 I2C 访问都在本线程；ISR 只累加中断计数。
 */
static void imu_sample_entry(void *parameter)
{
    uint32_t window_start = Bench_NowMs();
    uint32_t first_counter = 0U;
    uint32_t last_counter = 0U;
    uint8_t have_counter = 0U;
    uint32_t window_errors = 0U;
    uint32_t window_dropped = 0U;
    uint32_t last_edge = Imu_IntCount();
    uint32_t last_wake_ms = 0U;
    uint8_t last_state = (uint8_t)RD91_VEHICLE_UNKNOWN;

    (void)parameter;

    Rd91Motion_AccumReset(&motion_acc);
    Rd91Motion_JudgeReset(&motion_judge, last_state);
    Rd91Shock_Reset(&shock_state);

    for (;;)
    {
        ImuFrame frame;
        uint32_t now;
        uint32_t edges;
        int read_ret;
        uint8_t read_ok = 1U;

        rt_thread_delay(RD91_IMU_SAMPLE_PERIOD_TICKS);
        now = Bench_NowMs();

        /*
         * 读失败不能直接 continue：那样窗口永远不结束，I2C 长时间故障
         * 也永远不会判成 FAULT。这里只把错误计入本窗口，然后照常走
         * 唤醒检查和窗口结束判断。
         */
        read_ret = Imu_ReadFrame(&frame);
        if (read_ret == IMU_ERR_INCONSISTENT)
        {
            /* 读取期间芯片恰好更新：本帧可能混着新旧样本，直接丢弃。
             * 这是链路正常工作的表现，不算 I2C 错误。 */
            window_dropped++;
            read_ok = 0U;
        }
        else if (read_ret != IMU_OK)
        {
            read_ok = 0U;
            window_errors++;
        }
        else if ((frame.status0 & IMU_STATUS0_ADATA) == 0U)
        {
            /* 没有新样本：这份数据上次已经处理过，不能再计入统计。 */
            window_dropped++;
        }
        else
        {
            int32_t x_mg = Imu_RawToMg(frame.raw.x);
            int32_t y_mg = Imu_RawToMg(frame.raw.y);
            int32_t z_mg = Imu_RawToMg(frame.raw.z);
            uint32_t norm_mg;
            uint8_t saturated;
            uint8_t shock_before;
            Rd91ShockEvent shock_event;

            if (have_counter == 0U)
            {
                have_counter = 1U;
                first_counter = frame.sample_counter;
            }
            last_counter = frame.sample_counter;

            /*
             * |a| = sqrt(x^2 + y^2 + z^2)，单位 mg，含重力。
             * ±8g 量程下最大约 13856mg，远小于 uint16 上限。
             */
            norm_mg = Rd91_IsqrtU64((uint64_t)((int64_t)x_mg * x_mg +
                                               (int64_t)y_mg * y_mg +
                                               (int64_t)z_mg * z_mg));
            saturated = (Imu_RawSaturated(frame.raw.x) != 0 ||
                         Imu_RawSaturated(frame.raw.y) != 0 ||
                         Imu_RawSaturated(frame.raw.z) != 0) ? 1U : 0U;

            shock_before = shock_state.active;
            if (Rd91Shock_Update(&shock_state, now, (uint16_t)norm_mg, saturated,
                                 &shock_params, &shock_event) != 0)
            {
                post_shock_event(&shock_event, now);
            }

            /* 事件期间（含回落等待期）的样本不参与运动统计。 */
            if (shock_before == 0U && shock_state.active == 0U)
            {
                Rd91Motion_AccumAdd(&motion_acc, x_mg, y_mg, z_mg);
            }
        }

        /*
         * Any-Motion 唤醒事件：INT 边沿计数变化，或芯片状态位仍在报告。
         * 持续扰动时限流成一条日志；边沿仍然计数，只是不逐次上报。
         */
        edges = Imu_IntCount();
        if ((read_ok != 0U) &&
            ((edges != last_edge) || ((frame.status1 & IMU_STATUS1_ANYMOTION) != 0U)))
        {
            if ((uint32_t)(now - last_wake_ms) >= RD91_IMU_WAKE_REPORT_MIN_MS)
            {
                post_wake_event(((frame.status1 & IMU_STATUS1_ANYMOTION) != 0U) ? 1U : 0U,
                                frame.status1, now);
                last_wake_ms = now;
            }
        }
        last_edge = edges;

        if ((uint32_t)(now - window_start) >= RD91_MOTION_WINDOW_MS)
        {
            uint32_t samples = motion_acc.samples;
            uint32_t rms_mg = Rd91Motion_AccumRmsMg(&motion_acc);
            uint8_t state = Rd91Motion_Judge(&motion_judge, rms_mg, samples,
                                             window_errors, &motion_params);

            /*
             * 状态变化才上报，避免每秒刷一条相同的状态。
             * 只有投递成功才更新“已上报状态”：队列满时这一秒丢掉的事件，
             * 下一个窗口还会重试，业务层不会长期停在旧状态。
             */
            if (state != last_state)
            {
                if (post_vehicle_state(state, now) == 0)
                {
                    last_state = state;
                }
            }

#if RD91_IMU_DEBUG_STATS
            post_stats_event(rms_mg, samples,
                             (uint32_t)((last_counter - first_counter) & 0x00FFFFFFU),
                             window_dropped, window_errors, edges, state, now);
#endif

            Rd91Motion_AccumReset(&motion_acc);
            window_start = now;
            window_errors = 0U;
            window_dropped = 0U;
            first_counter = last_counter;
        }
    }
}

/* ---------- 日志 ---------- */

static const char *vehicle_name(uint8_t state)
{
    switch (state)
    {
        case (uint8_t)RD91_VEHICLE_STILL:  return "STILL";
        case (uint8_t)RD91_VEHICLE_MOVING: return "MOVING";
        case (uint8_t)RD91_VEHICLE_FAULT:  return "FAULT";
        default:                           return "UNKNOWN";
    }
}

/* 每秒一行运动统计。n 是参与算法的样本数，chip_n 是芯片样本计数增量。 */
static void print_imu_stats(const Rd91Event *event)
{
    rt_kprintf("[%s] IMU_MOTION_STAT t=%u n=%u rms=%umg enter=%u exit=%u "
               "state=%s chip_n=%u/%u drop=%u err=%u edges=%u\n",
               RD91_ROLE,
               (unsigned int)event->at_ms,
               (unsigned int)event->u.stats.valid_samples,
               (unsigned int)event->u.stats.rms_mg,
               (unsigned int)event->u.stats.enter_count,
               (unsigned int)event->u.stats.exit_count,
               vehicle_name(event->u.stats.state),
               (unsigned int)event->u.stats.chip_delta,
               (unsigned int)IMU_CHIP_SAMPLES_PER_WINDOW,
               (unsigned int)event->u.stats.dropped,
               (unsigned int)event->u.stats.errors,
               (unsigned int)event->u.stats.int_edges);
}

/* 每次独立冲击一行。peak_norm 是三轴合矢量 |a|，含重力。 */
static void print_imu_shock(const Rd91Event *event)
{
    rt_kprintf("[%s] SHOCK t=%u peak_norm=%umg dur=%ums over=%u sat=%u\n",
               RD91_ROLE,
               (unsigned int)event->at_ms,
               (unsigned int)event->u.shock.peak_mg,
               (unsigned int)event->u.shock.duration_ms,
               (unsigned int)event->u.shock.over_samples,
               (unsigned int)event->u.shock.saturated);
}

/* ---------- 台架声光自检 ---------- */

#if RD91_BENCH_ALARM_SELFTEST
static uint8_t bench_alarm_on;

/*
 * 台架自检入口：走与业务报警完全相同的 sink，验证的是同一个声光服务。
 * 只改声光服务的期望状态，不修改业务状态机（app->alarm 不变）。
 * 按键路径在 main 线程调用；也可以从调试器直接调用 Bench_AlarmSelfTest(0/1)。
 */
void Bench_AlarmSelfTest(int on)
{
    uint8_t i;

    bench_alarm_on = (on != 0) ? 1U : 0U;
    for (i = 0U; i < (uint8_t)RD91_OUT_COUNT; i++)
    {
        alarm_output_sink(i, bench_alarm_on);
    }
    rt_kprintf("[%s] BENCH_ALARM_SELFTEST on=%u\n", RD91_ROLE,
               (unsigned int)bench_alarm_on);
}

static void bench_alarm_toggle(void)
{
    Bench_AlarmSelfTest(bench_alarm_on == 0U);
}
#endif

/* ---------- 按键线程 ---------- */

static void input_entry(void *parameter)
{
    Button button;
    (void)parameter;

    Button_Init(&button, Bench_KeyPressed(), Bench_NowMs(), 30U);
    for (;;)
    {
        uint32_t now = Bench_NowMs();
        if (Button_Update(&button, Bench_KeyPressed(), now) == BUTTON_EVENT_PRESS)
        {
            Rd91Event event;
            event.type = (uint8_t)RD91_EV_BENCH_KEY;
            event.at_ms = now;
            event.request_id = 0U;
            event.u.keyfob.command = 0U;
            event.u.keyfob.source = 0U;
            event.u.keyfob.reserved = 0U;
            event.u.keyfob.session = 0U;
            post_event(&event);
        }
        rt_thread_delay(1); /* 1 tick = 10ms */
    }
}

int main(void)
{
    Rd91App app;
    Rd91Sink sink;
    Rd91Policy policy;
    rt_err_t result;
    int imu_ret;
    uint32_t last_heartbeat_ms;
    uint32_t last_drops = 0U;

    Bench_Init();
    rt_kprintf("BOOT role=%s os=RTT_3.1.4 mode=BENCH_STUB_IO ble=NOT_IMPLEMENTED "
               "sensors=IMU_ANYMOTION sample=100Hz fogger=LOOPBACK_SIMULATED "
               "alarm=LOCAL_VOICE_BUZZER_LAMP charge=LED_RG_I2C batt=CW2217_I2C\n", RD91_ROLE);

    /* 三路声光输出先关断，再启动声光线程；线程内部等待语音芯片上电初始化。 */
    AlarmOut_Init();

    /* IMU 失败不阻止启动：先保证 console/按键/状态机可以单独验收。 */
    imu_ret = imu_bringup();

    /*
     * 充电指示灯要在 imu_bringup() 之后：I2c1_Init() 在那里完成，
     * 这里的 SGM41513 探测走同一条总线。探测失败只记故障，不影响启动。
     */
    ChargeLed_Init();

    /* 电量计同样挂在 I2C1 上；初始化会确认 VERSION 并退出 shutdown 模式。 */
    BatteryGauge_Init();

    sink.log = log_sink;
    sink.keyfob_reply = keyfob_reply_sink;
    sink.fog_send = fog_send_sink;
    sink.alarm_output = alarm_output_sink;
    sink.link_action = link_action_sink;
    sink.display_update = display_sink;

    Rd91App_DefaultPolicy(&policy);
    Rd91App_Init(&app, RD91_ROLE, &sink, &policy, Bench_NowMs());

    result = rt_mq_init(&events, "events", event_pool, sizeof(Rd91Event),
                        sizeof(event_pool), RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        rt_kprintf("FAULT mq_init=%d\n", result);
        return -1;
    }

    result = rt_thread_init(&input_thread, "input", input_entry, RT_NULL,
                            input_stack, sizeof(input_stack), 15, 5);
    if (result != RT_EOK)
    {
        rt_kprintf("FAULT thread_init=%d\n", result);
        return -1;
    }
    result = rt_thread_startup(&input_thread);
    if (result != RT_EOK)
    {
        rt_kprintf("FAULT thread_start=%d\n", result);
        return -1;
    }

    /* 采样线程要在消息队列初始化之后启动：它一运行就会投递事件。 */
    if (imu_ret == IMU_OK)
    {
        result = rt_thread_init(&imu_thread, "imu", imu_sample_entry, RT_NULL,
                                imu_stack, sizeof(imu_stack), 8, 5);
        if (result == RT_EOK)
        {
            result = rt_thread_startup(&imu_thread);
        }
        if (result != RT_EOK)
        {
            rt_kprintf("[%s] FAULT imu_thread=%d\n", RD91_ROLE, result);
        }
    }

    last_heartbeat_ms = Bench_NowMs();

    for (;;)
    {
        Rd91Event event;
        Rd91Event tick;

        /*
         * 等事件，超时 1 tick（10ms）。超时即用来产生一次 TICK，
         * 保证业务定时不受事件数量影响；有队列事件时也至少每 10ms 走一次 TICK。
         */
        result = rt_mq_recv(&events, &event, sizeof(event), 1);
        if (result == RT_EOK)
        {
            if (event.type == (uint8_t)RD91_EV_BENCH_KEY)
            {
                /* 台架按键只做自检，不代表任何产品动作。 */
                rt_kprintf("[%s] BENCH_KEY\n", RD91_ROLE);
                Bench_LedToggle();
#if RD91_BENCH_ALARM_SELFTEST
                bench_alarm_toggle();
#endif
            }
            else if (event.type == (uint8_t)RD91_EV_IMU_STATS)
            {
                /* 统计只是诊断信息，不进入业务层。 */
                print_imu_stats(&event);
            }
            else
            {
                /* 冲击事件先打印原始数据，再交给业务层留下上下文。 */
                if (event.type == (uint8_t)RD91_EV_SHOCK)
                {
                    print_imu_shock(&event);
                }
                Rd91App_HandleEvent(&app, &event);
            }
        }
        else if (result != -RT_ETIMEOUT)
        {
            rt_kprintf("FAULT mq_recv=%d\n", result);
            rt_thread_delay(1);
        }

        tick.type = (uint8_t)RD91_EV_TICK;
        tick.at_ms = Bench_NowMs();
        tick.request_id = 0U;
        tick.u.keyfob.command = 0U;
        tick.u.keyfob.source = 0U;
        tick.u.keyfob.reserved = 0U;
        tick.u.keyfob.session = 0U;
        Rd91App_HandleEvent(&app, &tick);

        /* 充电指示灯：内部按 RD91_CHG_POLL_MS 限速，单次 I2C 事务 <1ms。 */
        ChargeLed_Poll();

        /*
         * 电量计：内部按 RD91_BATT_POLL_MS 限速。需要上报时把事件投回队列，
         * 由本线程下一轮作为 RD91_EV_BATTERY 处理，业务状态仍只在 RD91App 里改。
         */
        {
            Rd91Event batt;

            if (BatteryGauge_Poll(battery_charging_flag(), &batt) != 0U)
            {
                post_event(&batt);
            }
        }

        if ((uint32_t)(Bench_NowMs() - last_heartbeat_ms) >= 1000U)
        {
            last_heartbeat_ms = Bench_NowMs();
            rt_kprintf("[%s] HEARTBEAT t=%u\n", RD91_ROLE,
                       (unsigned int)last_heartbeat_ms);
        }

        if (queue_drops != last_drops)
        {
            last_drops = queue_drops;
            rt_kprintf("[%s] WARN queue_drop=%u\n", RD91_ROLE,
                       (unsigned int)last_drops);
        }
    }
}
