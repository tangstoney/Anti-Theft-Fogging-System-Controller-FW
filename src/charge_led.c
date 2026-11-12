/**
 * @file charge_led.c
 * @brief 充电/状态指示灯：I2C 采样、台架模拟、日志与灯色输出。
 *
 * 数据来源：SGM41513（I2C 0x1A）的 REG08/REG09，见 drivers/sgm41513.c。
 * 台架没有充电模块时可打开 RD91_BENCH_CHG_SELFTEST，用调试器模拟；
 * 模拟值走的是同一条判定与点灯路径，不是直接写灯。
 */
#include <rtthread.h>
#include "board.h"
#include "i2c1.h"
#include "project_config.h"
#include "charge_indicator.h"
#include "charge_led.h"
#include "sgm41513.h"

/* 总线失败状态只在变化时打印，避免每 500ms 刷一行。 */
#define BUS_STATE_UNKNOWN 0U
#define BUS_STATE_OK      1U
#define BUS_STATE_FAILED  2U

static const Rd91ChargeParams charge_params =
{
    RD91_CHG_DEBOUNCE_MS,
    RD91_CHG_FAULT_BLINK_MS
};

static Rd91ChargeJudge charge_judge;
static uint32_t last_poll_ms;
static uint8_t led_red;
static uint8_t led_green;
static uint8_t bus_state = BUS_STATE_UNKNOWN;

#if RD91_BENCH_CHG_SELFTEST
/* -1 = 使用真实 I2C；0..4 见 Bench_ChargeSimSet()。 */
static volatile int8_t sim_state = -1;
#endif

static const char *charge_name(uint8_t state)
{
    switch (state)
    {
        case (uint8_t)RD91_CHG_NO_INPUT:  return "NO_INPUT";
        case (uint8_t)RD91_CHG_CHARGING:  return "CHARGING";
        case (uint8_t)RD91_CHG_FULL:      return "FULL";
        case (uint8_t)RD91_CHG_DISABLED:  return "DISABLED";
        case (uint8_t)RD91_CHG_FAULT:     return "FAULT";
        default:                          return "UNKNOWN";
    }
}

static void sample_from_status(const Sgm41513Status *st, Rd91ChargeSample *s)
{
    s->valid = 1U;
    s->pg_stat = st->pg_stat;
    s->chrg_stat = st->chrg_stat;
    s->chrg_fault = st->chrg_fault;
    s->bat_fault = st->bat_fault;
    s->ntc_fault = st->ntc_fault;
}

void ChargeLed_Init(void)
{
    uint8_t pn = 0xFFU;
    uint8_t rev = 0U;

    Board_ChargeIoInit();
    Rd91Charge_Reset(&charge_judge);
    last_poll_ms = Bench_NowMs();
    led_red = 0U;
    led_green = 0U;
    bus_state = BUS_STATE_UNKNOWN;

    if (I2c1_IsReady() == 0)
    {
        rt_kprintf("[%s] FAULT charge_bus=NOT_INIT\n", RD91_ROLE);
        return;
    }

    /* 读 REG0B 确认器件在位：PN=0 是 SGM41513，1 是 SGM41513A/D。 */
    if (Sgm41513_ReadId(&pn, &rev) == SGM_OK)
    {
        bus_state = BUS_STATE_OK;
        rt_kprintf("[%s] SGM_READY addr=0x1A pn=%u rev=%u poll_ms=%u\n", RD91_ROLE,
                   (unsigned int)pn, (unsigned int)rev,
                   (unsigned int)RD91_CHG_POLL_MS);
    }
    else
    {
        bus_state = BUS_STATE_FAILED;
        rt_kprintf("[%s] FAULT sgm_probe=NO_ACK addr=0x1A\n", RD91_ROLE);
    }
}

void ChargeLed_Poll(void)
{
    uint32_t now = Bench_NowMs();
    Rd91ChargeSample sample;
    uint8_t red;
    uint8_t green;

    /*
     * 灯色每次调用都按“当前已去抖状态”重算并输出：main 循环约 100Hz，
     * 故障闪烁精度不受 500ms 采样周期限制；这里不访问 I2C，开销可忽略。
     */
    Rd91Charge_LedLevels(charge_judge.state, &charge_params, now, &red, &green);
    if ((red != led_red) || (green != led_green))
    {
        Board_ChargeLedDrive((int)red, (int)green);
        led_red = red;
        led_green = green;
    }

    if (I2c1_IsReady() == 0)
    {
        return;
    }
    if ((uint32_t)(now - last_poll_ms) < RD91_CHG_POLL_MS)
    {
        return;
    }
    last_poll_ms = now;

#if RD91_BENCH_CHG_SELFTEST
    if (sim_state >= 0)
    {
        /* 模拟值只填状态位，后续判定与真实读取完全一致。 */
        sample.valid = 1U;
        sample.pg_stat = 1U;
        sample.chrg_stat = 2U;
        sample.chrg_fault = 0U;
        sample.bat_fault = 0U;
        sample.ntc_fault = 0U;
        switch ((int)sim_state)
        {
            case 0: /* 无输入 */
                sample.pg_stat = 0U;
                sample.chrg_stat = 0U;
                break;
            case 1: /* 充电中 */
                break;
            case 2: /* 充满 */
                sample.chrg_stat = 3U;
                break;
            case 3: /* 充电故障（过温，充电被挂起） */
                sample.chrg_fault = 2U;
                break;
            default: /* 4 = 有输入但充电被禁用 */
                sample.chrg_stat = 0U;
                break;
        }
    }
    else
#endif
    {
        Sgm41513Status status;

        if (Sgm41513_ReadStatus(&status) == SGM_OK)
        {
            if (bus_state != BUS_STATE_OK)
            {
                rt_kprintf("[%s] SGM_RECOVERED addr=0x1A\n", RD91_ROLE);
                bus_state = BUS_STATE_OK;
            }
            sample_from_status(&status, &sample);

            if (Rd91Charge_Update(&charge_judge, &sample, now, &charge_params) != 0U)
            {
                if (charge_judge.state == (uint8_t)RD91_CHG_FAULT)
                {
                    rt_kprintf("[%s] CHARGE_STATE state=FAULT chrg=%u bat=%u ntc=%u "
                               "reg08=0x%02X reg09=0x%02X\n", RD91_ROLE,
                               (unsigned int)status.chrg_fault,
                               (unsigned int)status.bat_fault,
                               (unsigned int)status.ntc_fault,
                               (unsigned int)status.reg08,
                               (unsigned int)status.reg09);
                }
                else
                {
                    rt_kprintf("[%s] CHARGE_STATE state=%s\n", RD91_ROLE,
                               charge_name(charge_judge.state));
                }
            }
        }
        else
        {
            /* 读失败不能当成“无输入”或“充满”，按 UNKNOWN 保持灯灭。 */
            if (bus_state != BUS_STATE_FAILED)
            {
                rt_kprintf("[%s] FAULT sgm_read=NO_ACK addr=0x1A\n", RD91_ROLE);
                bus_state = BUS_STATE_FAILED;
            }
            sample.valid = 0U;
            sample.pg_stat = 0U;
            sample.chrg_stat = 0U;
            sample.chrg_fault = 0U;
            sample.bat_fault = 0U;
            sample.ntc_fault = 0U;
            (void)Rd91Charge_Update(&charge_judge, &sample, now, &charge_params);
        }
    }
}

uint8_t ChargeLed_GetState(void)
{
    return charge_judge.state;
}

#if RD91_BENCH_CHG_SELFTEST
void Bench_ChargeSimSet(int state)
{
    if (state < -1)
    {
        state = -1;
    }
    if (state > 4)
    {
        state = 4;
    }
    sim_state = (int8_t)state;
    rt_kprintf("[%s] BENCH_CHARGE_SIM state=%d (-1=I2C 0=none 1=charging 2=full 3=fault 4=disabled)\n",
               RD91_ROLE, state);
}
#endif
