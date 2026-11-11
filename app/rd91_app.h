/**
 * @file rd91_app.h
 * @brief 中控业务状态机：布撤防、车辆状态、报警、喷烟请求、链路与状态汇总。
 *
 * 单线程约束：Rd91App_HandleEvent 只能由业务线程（当前为 RTT main 线程）调用；
 * 状态只在内部修改。时间 now_ms 由外部传入，所有超时用无符号差值。
 */
#ifndef RD91_APP_H
#define RD91_APP_H

#include <stdint.h>
#include "rd91_types.h"
#include "impact.h"

/* 可标定/待确认参数。数值来源见各字段注释，不得当作最终产品指标。 */
typedef struct
{
    uint16_t impact_g_x100;        /* 高 g 阈值，1.50g=150；待标定（Q05） */
    uint16_t impact_pulse_max_ms;  /* 脉冲宽度上限，P1 §6.2 给出 <150ms */
    uint32_t impact_window_ms;     /* 冲击计数窗口；待确认 */
    uint8_t  impact_count;         /* 窗口内确认次数；">2 次"暂取 3，待确认 */
    uint32_t radar_verify_ms;      /* 新增人体后 Driver ID 校验窗口 T3，2~3s 待定 */
    uint32_t alarm_max_ms;         /* 报警持续时间，30min 为 DVT 前待标定值 */
    uint32_t fog_ack_timeout_ms;   /* 喷烟业务回执超时，1000ms 为台架演示值 */
    uint8_t  fog_auto_retry;       /* 产品重试规则未定义，固定 0；不得置 1 */
    int16_t  rssi_far_dbm;         /* 远场断开阈值；0 = 未标定，不主动断开 */
} Rd91Policy;

typedef struct
{
    const char *role;
    Rd91Sink sink;
    Rd91Policy policy;

    uint32_t now_ms;

    Rd91ArmState arm;
    uint8_t arm_source;
    Rd91VehicleState vehicle;

    Rd91AlarmState alarm;
    uint8_t alarm_cause;
    uint32_t alarm_started_ms;

    Rd91FogState fog;
    uint32_t fog_req_id;
    uint32_t fog_stop_id;
    uint32_t fog_sent_ms;
    uint32_t fog_accepted_ms;
    uint8_t fog_exec_kind;
    uint8_t fog_stop_sent;

    uint32_t request_seq;

    uint8_t intrusion_pending;
    uint32_t intrusion_deadline_ms;

    Rd91LinkState link[RD91_PEER_COUNT];
    int16_t rssi_dbm[RD91_PEER_COUNT];
    Rd91BatteryInfo battery[RD91_BATTERY_COUNT];

    Rd91ImpactWindow impact;
    uint8_t dirty;
} Rd91App;

/* 填入当前默认策略（含所有待标定项）。 */
void Rd91App_DefaultPolicy(Rd91Policy *policy);

/* sink/policy 可为 NULL：NULL sink 表示无输出，NULL policy 使用默认值。 */
void Rd91App_Init(Rd91App *app, const char *role, const Rd91Sink *sink,
                  const Rd91Policy *policy, uint32_t now_ms);

/* 处理一个输入事件；TICK 由业务循环周期投递（<=100ms）。 */
void Rd91App_HandleEvent(Rd91App *app, const Rd91Event *event);

/* 生成状态快照供显示/上报。 */
void Rd91App_GetStatus(const Rd91App *app, Rd91Status *status);

#endif /* RD91_APP_H */
