/**
 * @file rd91_types.h
 * @brief 中控业务层公共类型：状态维度、输入事件与输出接口。
 *
 * 本文件属于 app/，只依赖 stdint.h；不包含 RT-Thread、厂商头文件或 printf。
 * 事件由 src/ 适配层从队列投递，业务状态只在 main 线程内修改。
 */
#ifndef RD91_TYPES_H
#define RD91_TYPES_H

#include <stdint.h>

#define RD91_PEER_COUNT 3U

/* 传输/业务/执行结果。OK 只表示当前层接受，不表示物理动作完成。 */
typedef enum
{
    RD91_RESULT_OK = 0,
    RD91_RESULT_ACCEPTED,
    RD91_RESULT_REJECTED,
    RD91_RESULT_NOT_SUPPORTED,
    RD91_RESULT_TIMEOUT,
    RD91_RESULT_BUSY,
    RD91_RESULT_UNKNOWN
} Rd91Result;

/* 布撤防：目标是明确状态，禁止"翻转状态"。出厂默认撤防（Q01 已确认）。 */
typedef enum
{
    RD91_ARM_UNKNOWN = 0,
    RD91_ARM_DISARMED,
    RD91_ARM_ARMED
} Rd91ArmState;

typedef enum
{
    RD91_ARM_SOURCE_BOOT = 0,
    RD91_ARM_SOURCE_LOCAL,
    RD91_ARM_SOURCE_KEYFOB
} Rd91ArmSource;

/* 车辆状态：UNKNOWN/FAULT 不得当作停车。MOVING 表示“行驶”，由振动推断。 */
typedef enum
{
    RD91_VEHICLE_UNKNOWN = 0,
    RD91_VEHICLE_STILL,    /* 停车 */
    RD91_VEHICLE_MOVING,   /* 行驶 */
    RD91_VEHICLE_FAULT
} Rd91VehicleState;

/* Driver ID / 授权结果。鉴权机制未实现，默认 UNKNOWN。 */
typedef enum
{
    RD91_AUTH_UNKNOWN = 0,
    RD91_AUTH_AUTHORIZED,
    RD91_AUTH_UNAUTHORIZED
} Rd91AuthState;

typedef enum
{
    RD91_ALARM_IDLE = 0,
    RD91_ALARM_ACTIVE
} Rd91AlarmState;

typedef enum
{
    RD91_CAUSE_NONE = 0,
    RD91_CAUSE_IMPACT,
    RD91_CAUSE_TOW,
    RD91_CAUSE_INTRUSION,
    RD91_CAUSE_REMOTE
} Rd91AlarmCause;

/* 报警清除原因。清声光不等于撤回已执行的喷烟（Q09）。 */
typedef enum
{
    RD91_ALARM_CLEAR_NONE = 0,
    RD91_ALARM_CLEAR_USER,
    RD91_ALARM_CLEAR_REMOTE,
    RD91_ALARM_CLEAR_TIMEOUT
} Rd91AlarmClearReason;

/* 喷烟请求生命周期：传输成功 != 业务接受 != 执行结果。 */
typedef enum
{
    RD91_FOG_IDLE = 0,
    RD91_FOG_DECIDED,   /* 已决定，尚未交给传输 */
    RD91_FOG_SENT,      /* 仅传输层已发出 */
    RD91_FOG_ACCEPTED,  /* 收到执行端业务回执 */
    RD91_FOG_COMPLETED, /* 收到执行结果（可能是模拟） */
    RD91_FOG_REJECTED,
    RD91_FOG_TIMEOUT,
    RD91_FOG_FAILED
} Rd91FogState;

typedef enum
{
    RD91_EXEC_NONE = 0,
    RD91_EXEC_SIMULATED, /* 台架替身/回环，不能宣称真实喷烟 */
    RD91_EXEC_PHYSICAL
} Rd91ExecKind;

typedef enum
{
    RD91_FOG_STAGE_NONE = 0,
    RD91_FOG_STAGE_ACCEPTED,
    RD91_FOG_STAGE_REJECTED
} Rd91FogStage;

typedef enum
{
    RD91_FOG_ACTION_START = 0,
    RD91_FOG_ACTION_STOP
} Rd91FogAction;

typedef enum
{
    RD91_PEER_KEYFOB = 0,
    RD91_PEER_FOGGER,
    RD91_PEER_PHONE
} Rd91Peer;

/* 电量条目：主机自身 + 三个对端。 */
typedef enum
{
    RD91_BATTERY_LOCAL = 0,
    RD91_BATTERY_KEYFOB,
    RD91_BATTERY_FOGGER,
    RD91_BATTERY_PHONE,
    RD91_BATTERY_COUNT
} Rd91BatteryIndex;

typedef enum
{
    RD91_LINK_UNKNOWN = 0,
    RD91_LINK_OFFLINE,
    RD91_LINK_ONLINE
} Rd91LinkState;

typedef enum
{
    RD91_LINK_ACTION_DISCONNECT = 0,
    RD91_LINK_ACTION_CONNECT
} Rd91LinkAction;

typedef enum
{
    RD91_CMD_NONE = 0,
    RD91_CMD_ARM,
    RD91_CMD_DISARM,
    RD91_CMD_TRIGGER,
    RD91_CMD_CANCEL,
    RD91_CMD_PAIR
} Rd91Command;

typedef enum
{
    RD91_OUT_VOICE = 0,
    RD91_OUT_BUZZER,
    RD91_OUT_LAMP,
    RD91_OUT_COUNT
} Rd91Output;

/*
 * 本机声光输出状态。区分“命令已发出”和“确认在播放”：
 * 只有语音芯片 BUSY 反馈才能把 VOICE 置为 ON；GPIO 驱动的蜂鸣器/灯
 * 只能证明 MCU 已输出电平，不能证明真实声压/亮度（声压与亮度需实测）。
 */
typedef enum
{
    RD91_AOUT_OFF = 0,
    RD91_AOUT_CMD_SENT, /* 命令已完整发出，尚未得到播放确认 */
    RD91_AOUT_ON,       /* 已确认正在播放/输出 */
    RD91_AOUT_FAULT
} Rd91AudioState;

/*
 * 报警输出快照。version 对应最近一次“期望状态”变化，供后续通信线程
 * 等订阅者判断是否有新状态；每个订阅者应自己保存待发送记录，不能共用一份。
 * requested 是期望输出位图，voice/buzzer/lamp 是本机实际已下发到的状态。
 */
typedef struct
{
    uint32_t version;
    uint8_t requested; /* bit = Rd91Output */
    uint8_t voice;     /* Rd91AudioState */
    uint8_t buzzer;    /* 0/1：已驱动电平，不是声压确认 */
    uint8_t lamp;      /* 0/1：正在交替闪烁 */
    uint8_t fault;     /* 最近一次观测到的故障通道位图 */
    uint8_t reserved[3];
} Rd91AlarmSnapshot;

typedef struct
{
    uint8_t percent;        /* 0..100 */
    uint8_t percent_valid;  /* 0 = 无有效数据，显示未知 */
    uint8_t charging;
    uint8_t charging_valid;
} Rd91BatteryInfo;

/* 显示/上报用的状态快照。 */
typedef struct
{
    uint8_t arm;
    uint8_t arm_source;
    uint8_t vehicle;
    uint8_t alarm;
    uint8_t alarm_cause;
    uint8_t fog;
    uint8_t fog_exec_kind;
    uint8_t keyfob_paired;  /* 配对/绑定未实现，固定 0 */
    uint8_t fogger_paired;
    uint8_t phone_paired;
    uint8_t link[RD91_PEER_COUNT];
    Rd91BatteryInfo battery[RD91_BATTERY_COUNT];
    uint32_t fog_request_id;
} Rd91Status;

/* 输入事件类型。 */
typedef enum
{
    RD91_EV_NONE = 0,
    RD91_EV_TICK,
    RD91_EV_KEYFOB_COMMAND,
    RD91_EV_IMPACT,        /* 单次高 g 冲击采样 */
    RD91_EV_TOW,           /* 车辆运动分析给出的拖车事件 */
    RD91_EV_VEHICLE_STATE,
    RD91_EV_RADAR_PRESENCE,
    RD91_EV_AUTH_RESULT,
    RD91_EV_LINK_STATE,
    RD91_EV_RSSI,
    RD91_EV_BATTERY,
    RD91_EV_FOG_ACK,       /* 烟罐业务回执 */
    RD91_EV_FOG_RESULT,    /* 烟罐执行结果 */
    RD91_EV_MOTION,        /* IMU 芯片报“运动条件满足”，不是产品级冲击判定 */
    RD91_EV_SHOCK,         /* 软件逐样本检测出的“疑似冲击”，未经业务判定 */
    RD91_EV_IMU_STATS,     /* 每秒一条传感器统计，仅适配层用于日志 */
    RD91_EV_BENCH_KEY      /* 台架按键自检，仅适配层使用 */
} Rd91EventType;

typedef struct
{
    uint8_t type;   /* Rd91EventType */
    uint32_t at_ms;
    uint32_t request_id;
    union
    {
        struct
        {
            uint8_t command; /* Rd91Command */
            uint8_t source;  /* Rd91ArmSource */
            uint16_t reserved;
            uint32_t session;
        } keyfob;
        struct
        {
            uint16_t peak_g_x100; /* 1.50g => 150 */
            uint16_t pulse_ms;
        } impact;
        struct
        {
            uint8_t state; /* Rd91VehicleState */
            uint8_t reserved;
        } vehicle;
        struct
        {
            uint8_t present;
            uint8_t zone;
        } radar;
        struct
        {
            uint8_t state; /* Rd91AuthState */
            uint8_t reserved;
        } auth;
        struct
        {
            uint8_t peer;  /* Rd91Peer */
            uint8_t state; /* Rd91LinkState */
            int16_t rssi_dbm; /* 0 = 本次未提供 */
        } link;
        struct
        {
            uint8_t index; /* Rd91BatteryIndex */
            uint8_t percent;
            uint8_t charging;
            uint8_t valid;
        } battery;
        struct
        {
            uint8_t peer;
            uint8_t stage;     /* Rd91FogStage */
            uint8_t result;    /* Rd91Result */
            uint8_t exec_kind; /* Rd91ExecKind */
            uint32_t detail;
        } fog;
        struct
        {
            /*
             * any_motion = 1：读取时芯片 STATUS1 仍然报告“运动条件满足”。
             * any_motion = 0：只收到过中断边沿，条件已经撤销
             *                 （轻碰、短促扰动常见这种现象）。
             * 两种情况下都不能推断冲击峰值，峰值只由 RD91_EV_SHOCK 提供。
             */
            uint8_t any_motion;
            uint8_t status1_raw; /* 原始 STATUS1，仅用于日志定位 */
        } motion;
        struct
        {
            /*
             * 疑似冲击：软件按 10ms 采样逐样本检测，指标是
             * 三轴合矢量 |a|（含重力，静止约 1000mg），不是“冲击加速度本身”。
             * 峰值是采样点上观察到的最大值，不能承诺捕获任意短脉冲。
             */
            uint16_t peak_mg;      /* 事件期间 |a| 最大值，mg */
            uint16_t duration_ms;  /* 首个超阈值样本到最后一个回落样本 */
            uint16_t over_samples; /* 超过进入阈值的样本数 */
            uint8_t saturated;     /* 1 = 峰值被量程截断，不能当真值 */
        } shock;
        struct
        {
            /*
             * 每秒一条的运动统计，只给适配层打日志用，业务层不解释。
             * valid_samples 是真正参与算法的样本数（100Hz 下约 100）；
             * chip_delta 是芯片样本计数增量（1000Hz 下约 1000），
             * 两者比例体现“软件只取其中一部分样本”这一设计事实。
             * dropped 是丢弃帧数：读到 aDA=0（无新数据），或读取期间芯片更新、
             * 一致性复核不通过而整帧丢弃。静置时应接近 0；读取与 ODR 重叠时
             * 偶尔非 0 是正常现象，不代表故障。
             */
            uint16_t valid_samples;
            uint16_t rms_mg;
            uint16_t chip_delta;
            uint16_t dropped;
            uint32_t int_edges;
            uint8_t enter_count;
            uint8_t exit_count;
            uint8_t state;   /* Rd91VehicleState */
            uint8_t errors;  /* 窗口内 I2C 错误次数 */
        } stats;
    } u;
} Rd91Event;

/*
 * 输出接口。任何函数都不允许把"调用成功"当成业务或物理成功：
 * - fog_send 返回 OK 仅表示传输层已接受该请求；
 * - alarm_output 只登记期望状态并唤醒声光线程，不等待、不返回执行结果，
 *   真实发声/发光由声光线程完成，故障只通过日志和快照体现；
 * - link_action 在 P1 为桩，不做真实断连。
 */
typedef struct
{
    void (*log)(const char *event, int32_t v1, int32_t v2);
    void (*keyfob_reply)(uint32_t request_id, uint32_t session, uint8_t result, uint8_t arm_state);
    Rd91Result (*fog_send)(uint32_t request_id, uint8_t action, uint32_t at_ms);
    void (*alarm_output)(uint8_t output, uint8_t on);
    void (*link_action)(uint8_t peer, uint8_t action, int32_t arg);
    void (*display_update)(const Rd91Status *status);
} Rd91Sink;

#endif /* RD91_TYPES_H */
