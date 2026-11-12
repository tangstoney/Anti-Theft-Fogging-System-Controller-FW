/**
 * @file wt588f02.c
 * @brief WT588F02-8S-C 语音驱动：单线/两线串口 + GPIO + DWT 微秒延时。
 *
 * 控制方式由烧录语音工程时固定，用 RD91_WT_WIRE_MODE 选择：
 *   - 1 = 单线（当前交付）：只有 DATA 线，接芯片 IO2（PB0）。
 *       每字节：DATA 先高 >=5ms（必须有拉高动作）→ 拉低 5ms 起始 →
 *       8 bit，高电平在前、低电平在后；高:低 = 3:1 表示 1，
 *       1:3 表示 0；低位先发（§6.2.1/§6.2.4）。
 *   - 2 = 两线：CLK=IO2（PB0）、DATA=IO3（PB1），上升沿采样，低位先发
 *       （§6.2.5/§6.2.8）。
 * 两种方式的命令字相同（语音地址、F2、FE、E0..EF），命令间隔均 >=2ms。
 *
 * 为什么不用硬件定时器或 RTOS tick：
 *   - RTT tick 是 10ms，单线位宽是 200~600µs，tick 无法表达；
 *   - 位时序由本线程用 DWT 周期计数器忙等产生，连续 8 位期间调用
 *     rt_enter_critical() 只锁调度器、不关中断：SysTick 和 UART 中断照常，
 *     线程切换被推迟的时间上限是一个字节的 8 位（单线约 6.4ms）；
 *   - 字节前的 5ms 高/5ms 低等待放在锁外，其他线程可以运行。
 */
#include <rtthread.h>
#include "n32wb43x.h"
#include "board.h"
#include "project_config.h"
#include "wt588f02.h"

#if (RD91_WT_WIRE_MODE != 1) && (RD91_WT_WIRE_MODE != 2)
#error "RD91_WT_WIRE_MODE 只能是 1（单线）或 2（两线）"
#endif

#if (RD91_WT_WIRE_MODE == 1) &&                                                \
    ((RD91_WT_BIT_ONE_HIGH_US < 300U) || (RD91_WT_BIT_ONE_HIGH_US > 1200U) || \
     (RD91_WT_BIT_ONE_LOW_US < 100U) || (RD91_WT_BIT_ONE_LOW_US > 400U) ||    \
     (RD91_WT_BIT_ZERO_HIGH_US < 100U) || (RD91_WT_BIT_ZERO_HIGH_US > 400U) || \
     (RD91_WT_BIT_ZERO_LOW_US < 300U) || (RD91_WT_BIT_ZERO_LOW_US > 1200U))
#error "单线位时序短电平 100~400µs，长电平 300~1200µs"
#endif

#if (1000 % RT_TICK_PER_SECOND) != 0
#error "RD91_WT_INIT_MS 按 tick 取整，要求 1000 能被 RT_TICK_PER_SECOND 整除"
#endif

#if (RD91_WT_TWO_BYTE_ADDR == 0) && (RD91_WT_SEGMENT_ALARM > 0xDFU)
#error "单字节地址格式只支持 0..223 段"
#endif

#if (RD91_WT_TWO_BYTE_ADDR != 0) && (RD91_WT_SEGMENT_ALARM > 999U)
#error "双字节地址格式只支持 0..999 段"
#endif

#define WT_MS_TO_TICKS(ms) ((ms) / (1000U / RT_TICK_PER_SECOND))

/* DWT 每微秒的周期数（108MHz -> 108）。0 表示 DWT 不可用。 */
static uint32_t wt_cycles_per_us;

/* 微秒忙等。只看无符号差值，CYCCNT 回绕（约 39.8s）不影响小于一个回绕周期的延时。 */
static void wt_delay_us(uint32_t us)
{
    uint32_t start;
    uint32_t cycles;

    if (wt_cycles_per_us == 0U)
    {
        return;
    }
    start = DWT->CYCCNT;
    cycles = us * wt_cycles_per_us;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
    }
}

int Wt_ClockInit(void)
{
    /* TRCENA 使能 DWT；CyCCNTENA 使 CYCCNT 随内核时钟递增。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* 个别实现没有周期计数器：此时无法保证位时序，必须报错而不是盲发。 */
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        wt_cycles_per_us = 0U;
        return WT_ERR_INIT;
    }

    wt_cycles_per_us = SystemCoreClock / 1000000U;
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }
    return WT_OK;
}

uint32_t Wt_NowUs(void)
{
    if (wt_cycles_per_us == 0U)
    {
        return 0U;
    }
    return DWT->CYCCNT / wt_cycles_per_us;
}

int Wt_BusyLevel(void)
{
    return Board_WtBusyLevel();
}

/*
 * 发送一个字节。
 *
 * 单线（RD91_WT_WIRE_MODE=1，数据线 = 芯片 IO2 = PB0）：
 *   DATA 高 >=5ms -> 低 5ms 起始 -> 逐位“高在前、低在后”：
 *   1 = 高 600µs + 低 200µs，0 = 高 200µs + 低 600µs（资料推荐值）。
 *   芯片内部 220K 下拉，空闲为低；发码前必须有拉高 5ms 的动作。
 *
 * 两线（=2，CLK=PB0、DATA=PB1）：
 *   CLK 高 -> 等待 -> CLK 低（字节起始）-> 逐位：DATA 建立 -> CLK 上升沿采样 -> CLK 低。
 *   数据在 CLK 低电平期间改变，在上升沿时已经稳定。
 */
static void wt_send_byte(uint8_t value)
{
    uint8_t i;

#if RD91_WT_WIRE_MODE == 1
    /* 字节前：数据线先保持高 >=5ms（资料要求“必须先拉高”），再拉低 5ms 作为起始。 */
    Board_WtSetWire(1);
    wt_delay_us(RD91_WT_PRE_HIGH_US);
    Board_WtSetWire(0);
    wt_delay_us(RD91_WT_START_LOW_US);

    /*
     * 连续 8 位期间锁调度：防止线程切换把某个位拉长到超出芯片容限。
     * rt_enter_critical() 只增加调度器锁计数，不关全局中断，SysTick/UART 照常响应。
     * 8 位 x 800µs ≈ 6.4ms，是其他线程被推迟的上限。
     */
    rt_enter_critical();
    for (i = 0U; i < 8U; i++)
    {
        if (((value >> i) & 1U) != 0U) /* 低位先发 */
        {
            Board_WtSetWire(1);
            wt_delay_us(RD91_WT_BIT_ONE_HIGH_US);
            Board_WtSetWire(0);
            wt_delay_us(RD91_WT_BIT_ONE_LOW_US);
        }
        else
        {
            Board_WtSetWire(1);
            wt_delay_us(RD91_WT_BIT_ZERO_HIGH_US);
            Board_WtSetWire(0);
            wt_delay_us(RD91_WT_BIT_ZERO_LOW_US);
        }
    }
    rt_exit_critical();

    /* 恢复空闲低电平（芯片 IO 内部默认下拉，低电平是安全空闲态）。 */
    Board_WtSetWire(0);
#else
    /* 字节前：CLK 先保持高 >=5ms（资料要求“必须先拉高”），再拉低 5ms 作为起始。 */
    Board_WtSetClk(1);
    wt_delay_us(RD91_WT_PRE_HIGH_US);
    Board_WtSetClk(0);
    wt_delay_us(RD91_WT_START_LOW_US);

    /*
     * 连续 8 位期间锁调度：防止线程切换把某个位拉长到超出芯片容限。
     * 8 位 x 700µs ≈ 5.6ms，是其他线程被推迟的上限。
     */
    rt_enter_critical();
    for (i = 0U; i < 8U; i++)
    {
        Board_WtSetData((value >> i) & 1U); /* 低位先发 */
        wt_delay_us(RD91_WT_CLK_HALF_US);
        Board_WtSetClk(1);                  /* 上升沿采样 */
        wt_delay_us(RD91_WT_CLK_HALF_US);
        Board_WtSetClk(0);
    }
    rt_exit_critical();

    /* 恢复 DATA 低电平（芯片 IO 内部默认下拉，低电平是安全空闲态）。 */
    Board_WtSetData(0);
#endif
}

/* 单条命令字：单字节模式直接发；双字节模式先发 FF 再发命令（资料 §6.2.6）。 */
static void wt_send_cmd_byte(uint8_t cmd)
{
#if RD91_WT_TWO_BYTE_ADDR
    wt_send_byte(0xFFU);
#endif
    wt_send_byte(cmd);
}

/* 两条命令之间的最小间隔，资料 §6.2.6 注 5。 */
static void wt_cmd_gap(void)
{
    wt_delay_us(RD91_WT_CMD_GAP_MS * 1000U);
}

int Wt_SendPlay(uint16_t segment)
{
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }

#if RD91_WT_TWO_BYTE_ADDR
    if (segment > 999U)
    {
        return WT_ERR_PARAM;
    }
    wt_send_byte((uint8_t)(segment >> 8));
    wt_send_byte((uint8_t)(segment & 0xFFU));
#else
    if (segment > 0xDFU)
    {
        return WT_ERR_PARAM;
    }
    wt_send_byte((uint8_t)segment);
#endif

    wt_cmd_gap();
    return WT_OK;
}

int Wt_SendLoop(void)
{
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }
    wt_send_cmd_byte(0xF2U);
    wt_cmd_gap();
    return WT_OK;
}

int Wt_SendStop(void)
{
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }
    wt_send_cmd_byte(0xFEU);
    wt_cmd_gap();
    return WT_OK;
}

int Wt_SendVolume(uint8_t level)
{
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }
    if (level > 15U)
    {
        return WT_ERR_PARAM;
    }
    wt_send_cmd_byte((uint8_t)(0xE0U | level));
    wt_cmd_gap();
    return WT_OK;
}

int Wt_Init(void)
{
    if (wt_cycles_per_us == 0U)
    {
        return WT_ERR_INIT;
    }

    /* 资料 §2：上电初始化约 40ms，期间不响应指令。这里留余量。 */
    rt_thread_delay(WT_MS_TO_TICKS(RD91_WT_INIT_MS));

#if RD91_WT_VOLUME_LEVEL < 15U
    return Wt_SendVolume((uint8_t)RD91_WT_VOLUME_LEVEL);
#else
    return WT_OK;
#endif
}
