/**
 * @file battery_gauge.c
 * @brief 电量计采样、台架模拟、日志与业务事件生成。
 *
 * 数据来源：CW2217（I2C 0x64）的 VERSION/SOC/VCELL/TEMP，见 drivers/cw2217.c。
 * 台架没有电量计时可打开 RD91_BENCH_BATT_SELFTEST，用调试器模拟百分比；
 * 模拟值走的是同一条换算、过期判断与事件路径，不是直接改业务状态。
 */
#include <rtthread.h>
#include "board.h"
#include "i2c1.h"
#include "project_config.h"
#include "battery_level.h"
#include "battery_gauge.h"
#include "cw2217.h"

#define BUS_STATE_UNKNOWN 0U
#define BUS_STATE_OK      1U
#define BUS_STATE_FAILED  2U

static const Rd91BatteryParams battery_params =
{
    RD91_BATT_STALE_MS
};

static Rd91BatteryState battery_state;
static uint32_t last_poll_ms;
static uint32_t last_log_ms;
static uint8_t last_charging = RD91_BATT_CHARGING_UNKNOWN;
static uint8_t bus_state = BUS_STATE_UNKNOWN;

#if RD91_BENCH_BATT_SELFTEST
/* -1 = 使用真实 I2C；0..100 = 模拟百分比。 */
static volatile int8_t sim_percent = -1;
#endif

static void fill_event(Rd91Event *event, uint32_t now, uint8_t charging)
{
    event->type = (uint8_t)RD91_EV_BATTERY;
    event->at_ms = now;
    event->request_id = 0U;
    event->u.battery.index = (uint8_t)RD91_BATTERY_LOCAL;
    event->u.battery.percent = battery_state.percent;
    event->u.battery.charging = charging;
    event->u.battery.valid = battery_state.valid;
}

static void log_battery(const Rd91BatterySample *sample, uint8_t charging, int is_sim)
{
    int16_t tenths = Rd91Battery_TempTenthsC(sample->temp_raw);
    int t_int = (int)tenths / 10;
    int t_frac = (int)tenths % 10;

    if (t_frac < 0)
    {
        t_frac = -t_frac;
    }

    if (is_sim)
    {
        rt_kprintf("[%s] BATTERY_STAT src=SIM percent=%u valid=%u chg=%u\n", RD91_ROLE,
                   (unsigned int)battery_state.percent,
                   (unsigned int)battery_state.valid,
                   (unsigned int)charging);
        return;
    }

    rt_kprintf("[%s] BATTERY_STAT src=I2C percent=%u valid=%u chg=%u "
               "soc_raw=%u mv=%u t=%d.%d\n", RD91_ROLE,
               (unsigned int)battery_state.percent,
               (unsigned int)battery_state.valid,
               (unsigned int)charging,
               (unsigned int)sample->soc_raw,
               (unsigned int)Rd91Battery_VoltageMv(sample->vcell_raw),
               t_int, t_frac);
}

void BatteryGauge_Init(void)
{
    uint8_t version = 0U;
    int ret;

    Rd91Battery_Reset(&battery_state);
    last_poll_ms = Bench_NowMs();
    last_log_ms = last_poll_ms;
    bus_state = BUS_STATE_UNKNOWN;

    if (I2c1_IsReady() == 0)
    {
        rt_kprintf("[%s] FAULT battery_bus=NOT_INIT\n", RD91_ROLE);
        return;
    }

    ret = Cw2217_Probe(&version);
    if (ret != CW2217_OK)
    {
        bus_state = BUS_STATE_FAILED;
        rt_kprintf("[%s] FAULT cw2217_probe=%d addr=0x64 version=0x%02X\n", RD91_ROLE,
                   ret, (unsigned int)version);
        return;
    }

    /* 上电默认 shutdown：写 0x30、0x00 进入 active，电量才开始更新。 */
    ret = Cw2217_Wake();
    if (ret != CW2217_OK)
    {
        bus_state = BUS_STATE_FAILED;
        rt_kprintf("[%s] FAULT cw2217_wake=%d addr=0x64\n", RD91_ROLE, ret);
        return;
    }

    bus_state = BUS_STATE_OK;
    rt_kprintf("[%s] BATT_READY addr=0x64 version=0x%02X poll_ms=%u\n", RD91_ROLE,
               (unsigned int)version, (unsigned int)RD91_BATT_POLL_MS);
}

uint8_t BatteryGauge_Poll(uint8_t charging, Rd91Event *event)
{
    uint32_t now = Bench_NowMs();
    Rd91BatterySample sample;
    uint8_t report;
    int is_sim = 0;
    uint8_t charging_changed;

    if ((event == 0) || (I2c1_IsReady() == 0))
    {
        return 0U;
    }
    if ((uint32_t)(now - last_poll_ms) < RD91_BATT_POLL_MS)
    {
        return 0U;
    }
    last_poll_ms = now;

#if RD91_BENCH_BATT_SELFTEST
    if (sim_percent >= 0)
    {
        is_sim = 1;
        sample.valid = 1U;
        sample.soc_raw = (uint16_t)((uint16_t)sim_percent * 256U);
        sample.vcell_raw = 0U;
        sample.temp_raw = 0U;
    }
    else
#endif
    {
        Cw2217Sample raw;

        if (Cw2217_ReadSample(&raw) == CW2217_OK)
        {
            if (bus_state != BUS_STATE_OK)
            {
                rt_kprintf("[%s] CW2217_RECOVERED addr=0x64\n", RD91_ROLE);
                bus_state = BUS_STATE_OK;
            }

            sample.soc_raw = raw.soc_raw;
            sample.vcell_raw = raw.vcell_raw;
            sample.temp_raw = raw.temp_raw;
            sample.valid = Rd91Battery_SocValid(raw.soc_raw);

            if (sample.valid == 0U)
            {
                /* 超出 0..100% 的原始值不参与换算，按无效处理。 */
                rt_kprintf("[%s] WARN battery_soc_invalid raw=%u\n", RD91_ROLE,
                           (unsigned int)raw.soc_raw);
            }
        }
        else
        {
            /* 读失败不能当成 0%：本次采样无效，是否转为“未知”由过期时间决定。 */
            if (bus_state != BUS_STATE_FAILED)
            {
                rt_kprintf("[%s] FAULT cw2217_read=NO_ACK addr=0x64\n", RD91_ROLE);
                bus_state = BUS_STATE_FAILED;
            }
            sample.valid = 0U;
            sample.soc_raw = 0U;
            sample.vcell_raw = 0U;
            sample.temp_raw = 0U;
        }
    }

    report = Rd91Battery_Update(&battery_state, &sample, now, &battery_params);
    charging_changed = (charging != last_charging) ? 1U : 0U;

    if ((sample.valid != 0U) &&
        ((uint32_t)(now - last_log_ms) >= RD91_BATT_LOG_MS))
    {
        log_battery(&sample, charging, is_sim);
        last_log_ms = now;
    }

    if ((report == 0U) && (charging_changed == 0U))
    {
        return 0U;
    }

    last_charging = charging;
    fill_event(event, now, charging);
    rt_kprintf("[%s] BATTERY_REPORT percent=%u valid=%u chg=%u\n", RD91_ROLE,
               (unsigned int)battery_state.percent,
               (unsigned int)battery_state.valid,
               (unsigned int)charging);
    return 1U;
}

#if RD91_BENCH_BATT_SELFTEST
void Bench_BatterySimSet(int percent)
{
    if (percent < -1)
    {
        percent = -1;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    sim_percent = (int8_t)percent;
    rt_kprintf("[%s] BENCH_BATTERY_SIM percent=%d (-1=I2C 0..100)\n", RD91_ROLE, percent);
}
#endif
