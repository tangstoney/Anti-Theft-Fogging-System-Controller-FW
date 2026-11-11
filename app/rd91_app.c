/**
 * @file rd91_app.c
 * @brief 中控业务状态机实现。
 *
 * 已确认规则直接实现：
 * - 出厂默认撤防（Q01）；
 * - ARM/DISARM 是设定目标状态，不做翻转；
 * - 撤防、行驶（MOVING）、UNKNOWN 不产生新的报警/喷烟执行请求（R02/R03）；
 * - 喷烟只在 ARM + STILL + 报警激活时请求（R03）；
 * - 传输成功、业务回执、执行结果分开记录，超时/断线不标记成功（R09/Q08）；
 * - 喷烟默认不自动重发（Q09），重试策略未实现。
 *
 * 未确认规则不猜测：阈值、鉴权、撤销边界、重试次数均保留待确认标记。
 */
#include "rd91_app.h"

/* ---------- 小工具 ---------- */

static void app_log(const Rd91App *app, const char *event, int32_t v1, int32_t v2)
{
    if (app->sink.log != 0)
    {
        app->sink.log(event, v1, v2);
    }
}

static void mark_dirty(Rd91App *app)
{
    app->dirty = 1U;
}

/* 事件可能乱序到达（例如回环 ACK 的时间戳早于最近一次 TICK），
 * 只允许时间前进，避免超时判断因回绕被误触发。 */
static void time_advance(Rd91App *app, uint32_t at_ms)
{
    if ((int32_t)(at_ms - app->now_ms) > 0)
    {
        app->now_ms = at_ms;
    }
}

static uint32_t next_request_id(Rd91App *app)
{
    app->request_seq++;
    if (app->request_seq == 0U)
    {
        app->request_seq = 1U;
    }
    return app->request_seq;
}

static void alarm_outputs(Rd91App *app, uint8_t on)
{
    uint8_t i;

    if (app->sink.alarm_output == 0)
    {
        return;
    }
    for (i = 0U; i < (uint8_t)RD91_OUT_COUNT; i++)
    {
        app->sink.alarm_output(i, on);
    }
}

/* ---------- 喷烟请求 ---------- */

static int fog_in_flight(const Rd91App *app)
{
    return app->fog == RD91_FOG_DECIDED ||
           app->fog == RD91_FOG_SENT ||
           app->fog == RD91_FOG_ACCEPTED;
}

/* 决定与发送之间重新核对条件（module.md 要求）。 */
static int fog_preconditions_ok(const Rd91App *app)
{
    return app->arm == RD91_ARM_ARMED &&
           app->vehicle == RD91_VEHICLE_STILL &&
           app->alarm == RD91_ALARM_ACTIVE;
}

static void fog_request_start(Rd91App *app)
{
    uint32_t id;
    Rd91Result result;

    if (fog_in_flight(app))
    {
        app_log(app, "FOG_REQ_BUSY", (int32_t)app->fog, 0);
        return;
    }
    if (!fog_preconditions_ok(app))
    {
        app_log(app, "FOG_REQ_BLOCKED", (int32_t)app->arm, (int32_t)app->vehicle);
        return;
    }
    if (app->sink.fog_send == 0)
    {
        app->fog = RD91_FOG_FAILED;
        app->fog_exec_kind = (uint8_t)RD91_EXEC_NONE;
        app_log(app, "FOG_REQ_NO_TRANSPORT", 0, 0);
        mark_dirty(app);
        return;
    }

    id = next_request_id(app);
    app->fog_req_id = id;
    app->fog = RD91_FOG_DECIDED;
    app->fog_sent_ms = app->now_ms;
    app->fog_exec_kind = (uint8_t)RD91_EXEC_NONE;

    result = app->sink.fog_send(id, (uint8_t)RD91_FOG_ACTION_START, app->now_ms);
    if (result == RD91_RESULT_OK)
    {
        /* 仅传输层接受，不是业务接受，更不是喷烟成功。 */
        app->fog = RD91_FOG_SENT;
        app_log(app, "FOG_TRANSPORT_SENT", (int32_t)id, 0);
    }
    else
    {
        app->fog = RD91_FOG_FAILED;
        app_log(app, "FOG_SEND_FAILED", (int32_t)id, (int32_t)result);
    }
    mark_dirty(app);
}

static void fog_stop_request(Rd91App *app, uint8_t reason)
{
    uint32_t id;
    Rd91Result result;

    if (app->fog != RD91_FOG_SENT &&
        app->fog != RD91_FOG_ACCEPTED &&
        app->fog != RD91_FOG_COMPLETED)
    {
        return;
    }
    if (app->sink.fog_send == 0)
    {
        app_log(app, "FOG_STOP_NO_TRANSPORT", (int32_t)reason, 0);
        return;
    }

    id = next_request_id(app);
    app->fog_stop_id = id;
    result = app->sink.fog_send(id, (uint8_t)RD91_FOG_ACTION_STOP, app->now_ms);
    app->fog_stop_sent = (result == RD91_RESULT_OK) ? 1U : 0U;
    app_log(app, (result == RD91_RESULT_OK) ? "FOG_STOP_TRANSPORT_SENT" : "FOG_STOP_FAILED",
            (int32_t)id, (int32_t)reason);
    /* 停止请求不代表撤回已经执行的喷烟，不做状态回滚。 */
    mark_dirty(app);
}

/* ---------- 报警 ---------- */

static void alarm_start(Rd91App *app, uint8_t cause)
{
    if (app->alarm == RD91_ALARM_ACTIVE)
    {
        app_log(app, "ALARM_ALREADY_ACTIVE", (int32_t)cause, (int32_t)app->alarm_cause);
        return;
    }

    app->alarm = RD91_ALARM_ACTIVE;
    app->alarm_cause = cause;
    app->alarm_started_ms = app->now_ms;
    alarm_outputs(app, 1U);
    app_log(app, "ALARM_START", (int32_t)cause, (int32_t)app->now_ms);
    mark_dirty(app);

    /* 拖车只做声光报警，不喷烟（P1 §6.2，导图）。 */
    if (cause != (uint8_t)RD91_CAUSE_TOW)
    {
        fog_request_start(app);
    }
}

static void alarm_clear(Rd91App *app, uint8_t reason)
{
    if (app->alarm != RD91_ALARM_ACTIVE)
    {
        app_log(app, "ALARM_CLEAR_IGNORED", (int32_t)reason, 0);
        return;
    }

    app->alarm = RD91_ALARM_IDLE;
    app->alarm_cause = (uint8_t)RD91_CAUSE_NONE;
    alarm_outputs(app, 0U);
    app_log(app, "ALARM_CLEAR", (int32_t)reason, 0);
    mark_dirty(app);

    fog_stop_request(app, reason);

    /* 报警结束后本次喷烟请求生命周期终止；执行结果已在上报/日志中保留，
     * 显示不应再停留在 COMPLETED。停止请求不等于撤回已喷烟（Q09）。 */
    app->fog = RD91_FOG_IDLE;
    app->fog_exec_kind = (uint8_t)RD91_EXEC_NONE;
    mark_dirty(app);
}

/* ---------- 布撤防 ---------- */

static void arm_set(Rd91App *app, uint8_t target, uint8_t source)
{
    if (app->arm == (Rd91ArmState)target)
    {
        app_log(app, "ARM_UNCHANGED", (int32_t)target, (int32_t)source);
        return;
    }

    app->arm = (Rd91ArmState)target;
    app->arm_source = source;

    if (app->arm == RD91_ARM_DISARMED)
    {
        app->intrusion_pending = 0U;
        Rd91Impact_Reset(&app->impact);
    }

    app_log(app, "ARM_STATE", (int32_t)target, (int32_t)source);

    /* 撤防不自动清除已激活报警：撤销边界属 Q09，保持独立动作。 */
    if (app->arm == RD91_ARM_DISARMED && app->alarm == RD91_ALARM_ACTIVE)
    {
        app_log(app, "DISARM_ALARM_STILL_ACTIVE_TBD", 0, 0);
    }
    mark_dirty(app);
}

/* ---------- 车辆状态 ---------- */

static void vehicle_set(Rd91App *app, uint8_t state)
{
    if (state > (uint8_t)RD91_VEHICLE_FAULT)
    {
        /* 非法值不猜，按无法判断处理。 */
        state = (uint8_t)RD91_VEHICLE_UNKNOWN;
    }
    if (app->vehicle == (Rd91VehicleState)state)
    {
        return;
    }

    app->vehicle = (Rd91VehicleState)state;
    if (app->vehicle != RD91_VEHICLE_STILL)
    {
        app->intrusion_pending = 0U;
        Rd91Impact_Reset(&app->impact);
    }
    app_log(app, "VEHICLE_STATE", (int32_t)state, 0);
    mark_dirty(app);
}

/* ---------- 遥控指令 ---------- */

static void reply_keyfob(Rd91App *app, const Rd91Event *event, uint8_t result)
{
    app_log(app, "KEYFOB_REQ", (int32_t)event->u.keyfob.command, (int32_t)result);
    if (app->sink.keyfob_reply != 0)
    {
        app->sink.keyfob_reply(event->request_id, event->u.keyfob.session,
                               result, (uint8_t)app->arm);
    }
}

static void on_keyfob_command(Rd91App *app, const Rd91Event *event)
{
    uint8_t result = (uint8_t)RD91_RESULT_REJECTED;

    if (event->u.keyfob.source != (uint8_t)RD91_ARM_SOURCE_KEYFOB ||
        event->request_id == 0U)
    {
        /* 来源/请求号不合法：不改变状态，只回拒绝。 */
        reply_keyfob(app, event, (uint8_t)RD91_RESULT_REJECTED);
        return;
    }

    switch (event->u.keyfob.command)
    {
        case RD91_CMD_ARM:
            arm_set(app, (uint8_t)RD91_ARM_ARMED, (uint8_t)RD91_ARM_SOURCE_KEYFOB);
            result = (uint8_t)RD91_RESULT_OK;
            break;

        case RD91_CMD_DISARM:
            arm_set(app, (uint8_t)RD91_ARM_DISARMED, (uint8_t)RD91_ARM_SOURCE_KEYFOB);
            result = (uint8_t)RD91_RESULT_OK;
            break;

        case RD91_CMD_TRIGGER:
            if (app->arm != RD91_ARM_ARMED)
            {
                app_log(app, "TRIGGER_REJECTED_DISARMED", 0, 0);
            }
            else if (app->vehicle != RD91_VEHICLE_STILL)
            {
                app_log(app, "TRIGGER_REJECTED_VEHICLE", (int32_t)app->vehicle, 0);
            }
            else
            {
                alarm_start(app, (uint8_t)RD91_CAUSE_REMOTE);
                result = (uint8_t)RD91_RESULT_OK;
            }
            break;

        case RD91_CMD_CANCEL:
            /* 幂等：没有报警时也回 OK，但不清除其他状态。 */
            alarm_clear(app, (uint8_t)RD91_ALARM_CLEAR_REMOTE);
            result = (uint8_t)RD91_RESULT_OK;
            break;

        case RD91_CMD_PAIR:
            /* 配对/绑定/密钥未实现，不得返回成功。 */
            app_log(app, "PAIR_NOT_IMPLEMENTED", 0, 0);
            result = (uint8_t)RD91_RESULT_NOT_SUPPORTED;
            break;

        default:
            result = (uint8_t)RD91_RESULT_REJECTED;
            break;
    }

    reply_keyfob(app, event, result);
}

/* ---------- 传感器事件 ---------- */

static void on_impact(Rd91App *app, const Rd91Event *event)
{
    uint16_t g = event->u.impact.peak_g_x100;
    uint16_t pulse = event->u.impact.pulse_ms;
    uint8_t count;

    if (!Rd91Impact_IsValid(g, pulse, app->policy.impact_g_x100,
                            app->policy.impact_pulse_max_ms))
    {
        app_log(app, "IMPACT_FILTERED", (int32_t)g, (int32_t)pulse);
        return;
    }
    if (app->arm != RD91_ARM_ARMED)
    {
        app_log(app, "IMPACT_IGNORED_DISARMED", (int32_t)g, (int32_t)pulse);
        return;
    }
    if (app->vehicle != RD91_VEHICLE_STILL)
    {
        /* 行驶/未知/故障不产生新的报警与执行请求。 */
        app_log(app, "IMPACT_IGNORED_VEHICLE", (int32_t)app->vehicle, 0);
        return;
    }

    count = Rd91Impact_Accumulate(&app->impact, app->now_ms,
                                  app->policy.impact_window_ms);
    app_log(app, "IMPACT_VALID", (int32_t)count, (int32_t)g);

    if (count >= app->policy.impact_count)
    {
        Rd91Impact_Reset(&app->impact);
        app_log(app, "IMPACT_CONFIRMED", (int32_t)count, 0);
        alarm_start(app, (uint8_t)RD91_CAUSE_IMPACT);
    }
}

/* ---------- IMU 运动事件 ---------- */

/*
 * 驱动上报的是“芯片检测到运动条件满足”，不是“砸车/撬车/偷车”。
 *
 * 本轮刻意不升级为报警：
 *   - Any-Motion 阈值还是实验值，没有装车标定；
 *   - 安装位置、加装重量对触发结果的影响未知（Q05 待确认）；
 *   - 一旦接进 alarm_start 就会真的驱动声光和喷烟请求，风险不对等。
 * 所以这里只记日志。等阈值和判定规则确认后，再把这里换成真正的判定。
 */
static void on_motion(Rd91App *app, const Rd91Event *event)
{
    uint8_t any_motion = event->u.motion.any_motion;
    uint8_t status1 = event->u.motion.status1_raw;

    app_log(app, "IMU_MOTION_EVENT", (int32_t)any_motion, (int32_t)status1);

    if (any_motion != 0U)
    {
        app_log(app, "IMU_ANYMOTION_ASSERTED", (int32_t)status1, (int32_t)app->vehicle);
    }
    else
    {
        /* 读到的时候条件已撤销：只能证明“刚才有过中断边沿”，
         * 拿不到当时的加速度，也不能当成峰值上报。 */
        app_log(app, "IMU_INT_EDGE_ONLY", (int32_t)status1, 0);
    }

    /* 只是把上下文打出来，方便对照：布防 + 静停时的扰动才值得后续关注。 */
    if (app->arm == RD91_ARM_ARMED && app->vehicle == RD91_VEHICLE_STILL)
    {
        app_log(app, "IMU_DISTURBANCE_ARMED_TBD", (int32_t)status1, 0);
    }
}

/* ---------- 疑似冲击事件 ---------- */

/*
 * 采样层上报的是“软件在 10ms 采样点上看到 |a| 超过阈值”，不是“砸车/碰撞”。
 *   - 阈值是实验值，没有装车标定；
 *   - |a| 含重力，路面接缝、颠簸同样可能超过阈值；
 *   - 缓慢撬动没有大尖峰，不会产生事件（已知限制）。
 * 本轮只记录业务上下文，不进入 alarm_start，也不产生喷烟请求。
 * 峰值、时长、饱和标志由适配层直接打印，这里不重复。
 */
static void on_shock(Rd91App *app, const Rd91Event *event)
{
    if (app->arm == RD91_ARM_ARMED && app->vehicle == RD91_VEHICLE_STILL)
    {
        app_log(app, "SHOCK_ARMED_STILL_TBD",
                (int32_t)event->u.shock.peak_mg, (int32_t)event->u.shock.duration_ms);
    }
}

static void on_tow(Rd91App *app)
{
    if (app->arm != RD91_ARM_ARMED)
    {
        app_log(app, "TOW_IGNORED_DISARMED", 0, 0);
        return;
    }
    if (app->vehicle != RD91_VEHICLE_MOVING)
    {
        /* 拖车的完整判定规则待产品确认（Q05），当前只接受明确 MOVING（行驶）。 */
        app_log(app, "TOW_IGNORED_VEHICLE_TBD", (int32_t)app->vehicle, 0);
        return;
    }
    alarm_start(app, (uint8_t)RD91_CAUSE_TOW);
}

static void on_radar(Rd91App *app, const Rd91Event *event)
{
    if (!event->u.radar.present)
    {
        if (app->intrusion_pending)
        {
            app->intrusion_pending = 0U;
            app_log(app, "RADAR_CLEARED", 0, 0);
        }
        return;
    }

    if (app->arm != RD91_ARM_ARMED)
    {
        app_log(app, "RADAR_IGNORED_DISARMED", 0, 0);
        return;
    }
    if (app->vehicle != RD91_VEHICLE_STILL)
    {
        app_log(app, "RADAR_IGNORED_VEHICLE", (int32_t)app->vehicle, 0);
        return;
    }
    if (app->intrusion_pending)
    {
        /* 保留首次截止时间，避免持续有人时无限延长校验窗口。 */
        return;
    }

    app->intrusion_pending = 1U;
    app->intrusion_deadline_ms = app->now_ms + app->policy.radar_verify_ms;
    app_log(app, "AUTH_VERIFY_START", (int32_t)app->policy.radar_verify_ms, 0);
}

static void on_auth_result(Rd91App *app, const Rd91Event *event)
{
    if (!app->intrusion_pending)
    {
        app_log(app, "AUTH_UNSOLICITED", (int32_t)event->u.auth.state, 0);
        return;
    }
    app->intrusion_pending = 0U;

    switch (event->u.auth.state)
    {
        case RD91_AUTH_AUTHORIZED:
            app_log(app, "AUTH_OK", 0, 0);
            break;

        case RD91_AUTH_UNAUTHORIZED:
            app_log(app, "AUTH_FAILED", 0, 0);
            if (app->arm == RD91_ARM_ARMED && app->vehicle == RD91_VEHICLE_STILL)
            {
                alarm_start(app, (uint8_t)RD91_CAUSE_INTRUSION);
            }
            else
            {
                app_log(app, "INTRUSION_STALE", (int32_t)app->arm, (int32_t)app->vehicle);
            }
            break;

        default:
            /* 鉴权机制未实现（Q12）：结果未知时不报警、不执行，也不伪造授权。 */
            app_log(app, "AUTH_UNKNOWN_NO_ACTION_TBD", 0, 0);
            break;
    }
}

/* ---------- 链路与电量 ---------- */

static void on_link_state(Rd91App *app, const Rd91Event *event)
{
    uint8_t peer = event->u.link.peer;

    if (peer >= (uint8_t)RD91_PEER_COUNT)
    {
        app_log(app, "LINK_BAD_PEER", (int32_t)peer, 0);
        return;
    }
    app->link[peer] = (Rd91LinkState)event->u.link.state;
    if (event->u.link.rssi_dbm != 0)
    {
        app->rssi_dbm[peer] = event->u.link.rssi_dbm;
    }
    app_log(app, "LINK_STATE", ((int32_t)peer << 8) | (int32_t)event->u.link.state,
            (int32_t)event->u.link.rssi_dbm);
    mark_dirty(app);
}

static void on_rssi(Rd91App *app, const Rd91Event *event)
{
    uint8_t peer = event->u.link.peer;

    if (peer >= (uint8_t)RD91_PEER_COUNT)
    {
        return;
    }
    app->rssi_dbm[peer] = event->u.link.rssi_dbm;
    app_log(app, "RSSI", (int32_t)peer, (int32_t)event->u.link.rssi_dbm);

    /* RSSI 不能单独鉴权，也不能换算米；只有显式标定阈值后才请求断开。 */
    if (peer == (uint8_t)RD91_PEER_KEYFOB && app->policy.rssi_far_dbm != 0 &&
        event->u.link.rssi_dbm <= app->policy.rssi_far_dbm)
    {
        app_log(app, "RSSI_FAR_DISCONNECT_REQ", (int32_t)event->u.link.rssi_dbm,
                (int32_t)app->policy.rssi_far_dbm);
        if (app->sink.link_action != 0)
        {
            app->sink.link_action(peer, (uint8_t)RD91_LINK_ACTION_DISCONNECT,
                                  (int32_t)event->u.link.rssi_dbm);
        }
    }
}

static void on_battery(Rd91App *app, const Rd91Event *event)
{
    uint8_t index = event->u.battery.index;

    if (index >= (uint8_t)RD91_BATTERY_COUNT)
    {
        return;
    }

    if (event->u.battery.valid && event->u.battery.percent <= 100U)
    {
        app->battery[index].percent = event->u.battery.percent;
        app->battery[index].percent_valid = 1U;
    }
    else
    {
        /* 无效/过期数据不伪装成正常电量。 */
        app->battery[index].percent_valid = 0U;
    }

    if (event->u.battery.charging <= 1U)
    {
        app->battery[index].charging = event->u.battery.charging;
        app->battery[index].charging_valid = 1U;
    }
    else
    {
        app->battery[index].charging_valid = 0U;
    }

    app_log(app, "BATTERY", ((int32_t)index << 8) | (int32_t)event->u.battery.percent,
            (int32_t)event->u.battery.valid);
    mark_dirty(app);
}

/* ---------- 烟罐回执 ---------- */

static void on_fog_ack(Rd91App *app, const Rd91Event *event)
{
    uint8_t stage = event->u.fog.stage;

    if (event->u.fog.peer != (uint8_t)RD91_PEER_FOGGER)
    {
        app_log(app, "FOG_ACK_FOREIGN", (int32_t)event->u.fog.peer, 0);
        return;
    }
    if (app->fog_stop_id != 0U && event->request_id == app->fog_stop_id)
    {
        app_log(app, "FOG_STOP_ACK", (int32_t)stage, (int32_t)event->u.fog.result);
        return;
    }
    if (event->request_id != app->fog_req_id)
    {
        /* 旧包/错端包不得改变新请求状态。 */
        app_log(app, "FOG_ACK_STALE", (int32_t)event->request_id, (int32_t)app->fog_req_id);
        return;
    }
    if (stage == (uint8_t)RD91_FOG_STAGE_REJECTED)
    {
        app->fog = RD91_FOG_REJECTED;
        app_log(app, "FOG_REJECTED", (int32_t)event->u.fog.result, 0);
        mark_dirty(app);
        return;
    }
    if (app->fog != RD91_FOG_SENT)
    {
        app_log(app, "FOG_ACK_UNEXPECTED", (int32_t)app->fog, (int32_t)stage);
        return;
    }

    app->fog = RD91_FOG_ACCEPTED;
    app->fog_accepted_ms = app->now_ms;
    app_log(app, "FOG_BUSINESS_ACCEPTED", (int32_t)event->request_id,
            (int32_t)event->u.fog.result);
    mark_dirty(app);
}

static void on_fog_result(Rd91App *app, const Rd91Event *event)
{
    if (event->u.fog.peer != (uint8_t)RD91_PEER_FOGGER)
    {
        app_log(app, "FOG_RESULT_FOREIGN", (int32_t)event->u.fog.peer, 0);
        return;
    }
    if (app->fog_stop_id != 0U && event->request_id == app->fog_stop_id)
    {
        app_log(app, "FOG_STOP_RESULT", (int32_t)event->u.fog.result, 0);
        return;
    }
    if (event->request_id != app->fog_req_id)
    {
        app_log(app, "FOG_RESULT_STALE", (int32_t)event->request_id,
                (int32_t)app->fog_req_id);
        return;
    }
    if (app->fog != RD91_FOG_ACCEPTED && app->fog != RD91_FOG_SENT)
    {
        app_log(app, "FOG_RESULT_UNEXPECTED", (int32_t)app->fog, 0);
        return;
    }

    app->fog = RD91_FOG_COMPLETED;
    app->fog_exec_kind = event->u.fog.exec_kind;

    if (event->u.fog.exec_kind == (uint8_t)RD91_EXEC_SIMULATED)
    {
        /* 台架替身/回环结果：绝不显示为真实喷烟成功。 */
        app_log(app, "FOG_RESULT_SIMULATED", (int32_t)event->request_id,
                (int32_t)event->u.fog.detail);
    }
    else if (event->u.fog.exec_kind == (uint8_t)RD91_EXEC_PHYSICAL)
    {
        app_log(app, "FOG_RESULT_PHYSICAL", (int32_t)event->request_id,
                (int32_t)event->u.fog.detail);
    }
    else
    {
        app_log(app, "FOG_RESULT_UNKNOWN", (int32_t)event->request_id, 0);
    }
    mark_dirty(app);
}

/* ---------- 周期检查 ---------- */

static void on_tick(Rd91App *app)
{
    uint32_t elapsed;

    if (app->alarm == RD91_ALARM_ACTIVE)
    {
        elapsed = app->now_ms - app->alarm_started_ms;
        if (elapsed >= app->policy.alarm_max_ms)
        {
            alarm_clear(app, (uint8_t)RD91_ALARM_CLEAR_TIMEOUT);
        }
    }

    if (app->intrusion_pending)
    {
        if ((int32_t)(app->now_ms - app->intrusion_deadline_ms) >= 0)
        {
            app->intrusion_pending = 0U;
            /* 校验超时视为结果未知：不报警、不执行（待产品确认异常处理）。 */
            app_log(app, "AUTH_VERIFY_TIMEOUT", 0, 0);
        }
    }

    if (app->fog == RD91_FOG_SENT)
    {
        elapsed = app->now_ms - app->fog_sent_ms;
        if (elapsed >= app->policy.fog_ack_timeout_ms)
        {
            app->fog = RD91_FOG_TIMEOUT;
            /* 超时只上报，不自动重发（Q09）。 */
            app_log(app, "FOG_TIMEOUT", (int32_t)app->fog_req_id, 0);
            mark_dirty(app);
        }
    }
    else if (app->fog == RD91_FOG_ACCEPTED)
    {
        elapsed = app->now_ms - app->fog_accepted_ms;
        if (elapsed >= app->policy.fog_ack_timeout_ms)
        {
            app->fog = RD91_FOG_TIMEOUT;
            app_log(app, "FOG_RESULT_TIMEOUT", (int32_t)app->fog_req_id, 0);
            mark_dirty(app);
        }
    }
}

/* ---------- 对外接口 ---------- */

void Rd91App_DefaultPolicy(Rd91Policy *policy)
{
    policy->impact_g_x100 = 150U;             /* 1.50g，待标定 Q05 */
    policy->impact_pulse_max_ms = 150U;       /* P1 §6.2 */
    policy->impact_window_ms = 1000U;         /* 待确认 */
    policy->impact_count = 3U;                /* ">2 次"暂取 3，待确认 */
    policy->radar_verify_ms = 2500U;          /* T3 2~3s 取中值，待确认 */
    policy->alarm_max_ms = 30U * 60U * 1000U; /* 30min，DVT 前重标定 */
    policy->fog_ack_timeout_ms = 1000U;       /* 台架演示值 */
    policy->fog_auto_retry = 0U;              /* 未定义产品策略，禁止重发 */
    policy->rssi_far_dbm = 0;                 /* 0 = 未标定，不主动断开 */
}

void Rd91App_Init(Rd91App *app, const char *role, const Rd91Sink *sink,
                  const Rd91Policy *policy, uint32_t now_ms)
{
    uint8_t i;

    app->role = role;
    if (sink != 0)
    {
        app->sink = *sink;
    }
    else
    {
        app->sink.log = 0;
        app->sink.keyfob_reply = 0;
        app->sink.fog_send = 0;
        app->sink.alarm_output = 0;
        app->sink.link_action = 0;
        app->sink.display_update = 0;
    }
    if (policy != 0)
    {
        app->policy = *policy;
    }
    else
    {
        Rd91App_DefaultPolicy(&app->policy);
    }

    app->now_ms = now_ms;

    /* Q01 已确认：出厂默认撤防。车辆状态未知，不得当作静停。 */
    app->arm = RD91_ARM_DISARMED;
    app->arm_source = (uint8_t)RD91_ARM_SOURCE_BOOT;
    app->vehicle = RD91_VEHICLE_UNKNOWN;

    app->alarm = RD91_ALARM_IDLE;
    app->alarm_cause = (uint8_t)RD91_CAUSE_NONE;
    app->alarm_started_ms = now_ms;

    app->fog = RD91_FOG_IDLE;
    app->fog_req_id = 0U;
    app->fog_stop_id = 0U;
    app->fog_sent_ms = 0U;
    app->fog_accepted_ms = 0U;
    app->fog_exec_kind = (uint8_t)RD91_EXEC_NONE;
    app->fog_stop_sent = 0U;

    app->request_seq = 0U;
    app->intrusion_pending = 0U;
    app->intrusion_deadline_ms = 0U;

    for (i = 0U; i < (uint8_t)RD91_PEER_COUNT; i++)
    {
        app->link[i] = RD91_LINK_UNKNOWN;
        app->rssi_dbm[i] = 0;
    }
    for (i = 0U; i < (uint8_t)RD91_BATTERY_COUNT; i++)
    {
        app->battery[i].percent = 0U;
        app->battery[i].percent_valid = 0U;
        app->battery[i].charging = 0U;
        app->battery[i].charging_valid = 0U;
    }

    Rd91Impact_Reset(&app->impact);
    app->dirty = 0U;

    app_log(app, "APP_BOOT", (int32_t)app->arm, (int32_t)app->vehicle);

    if (app->sink.display_update != 0)
    {
        Rd91Status status;
        Rd91App_GetStatus(app, &status);
        app->sink.display_update(&status);
    }
}

void Rd91App_HandleEvent(Rd91App *app, const Rd91Event *event)
{
    if (app == 0 || event == 0)
    {
        return;
    }

    time_advance(app, event->at_ms);

    switch (event->type)
    {
        case RD91_EV_TICK:
            on_tick(app);
            break;
        case RD91_EV_KEYFOB_COMMAND:
            on_keyfob_command(app, event);
            break;
        case RD91_EV_IMPACT:
            on_impact(app, event);
            break;
        case RD91_EV_MOTION:
            on_motion(app, event);
            break;
        case RD91_EV_SHOCK:
            on_shock(app, event);
            break;
        case RD91_EV_TOW:
            on_tow(app);
            break;
        case RD91_EV_VEHICLE_STATE:
            vehicle_set(app, event->u.vehicle.state);
            break;
        case RD91_EV_RADAR_PRESENCE:
            on_radar(app, event);
            break;
        case RD91_EV_AUTH_RESULT:
            on_auth_result(app, event);
            break;
        case RD91_EV_LINK_STATE:
            on_link_state(app, event);
            break;
        case RD91_EV_RSSI:
            on_rssi(app, event);
            break;
        case RD91_EV_BATTERY:
            on_battery(app, event);
            break;
        case RD91_EV_FOG_ACK:
            on_fog_ack(app, event);
            break;
        case RD91_EV_FOG_RESULT:
            on_fog_result(app, event);
            break;
        default:
            break;
    }

    if (app->dirty)
    {
        Rd91Status status;
        app->dirty = 0U;
        Rd91App_GetStatus(app, &status);
        if (app->sink.display_update != 0)
        {
            app->sink.display_update(&status);
        }
    }
}

void Rd91App_GetStatus(const Rd91App *app, Rd91Status *status)
{
    uint8_t i;

    status->arm = (uint8_t)app->arm;
    status->arm_source = app->arm_source;
    status->vehicle = (uint8_t)app->vehicle;
    status->alarm = (uint8_t)app->alarm;
    status->alarm_cause = app->alarm_cause;
    status->fog = (uint8_t)app->fog;
    status->fog_exec_kind = app->fog_exec_kind;
    status->keyfob_paired = 0U; /* 配对/绑定未实现（Q12） */
    status->fogger_paired = 0U;
    status->phone_paired = 0U;
    status->fog_request_id = app->fog_req_id;

    for (i = 0U; i < (uint8_t)RD91_PEER_COUNT; i++)
    {
        status->link[i] = (uint8_t)app->link[i];
    }
    for (i = 0U; i < (uint8_t)RD91_BATTERY_COUNT; i++)
    {
        status->battery[i] = app->battery[i];
    }
}
