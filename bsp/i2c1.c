/**
 * @file i2c1.c
 * @brief I2C1 主机阻塞式轮询传输。
 *        时序严格对照 SDK 例程
 *        projects/n32wb43x_EVAL/examples/I2C/I2C_Master/src/main.c，
 *        并补上例程没有的错误收尾。
 *
 * 关键约定（与 SDK 一致，不要自行简化）：
 * - I2C_SendAddr7bit() 把参数原样写进 DAT，只改 bit0 决定读写。
 *   所以传入的必须是 **8 位形式** (addr7 << 1)，本文件在 i2c1_send_addr() 里统一左移。
 *   直接把 7 位地址传进去会访问到 addr7>>1 这个不存在的器件。
 * - 重复 START 时总线本来就处于 BUSY，不能再等 BUSY 清零；
 *   首次 START 才需要等总线空闲。
 * - 读方向要自己控制 ACK/ADDR/STOP 的顺序：等地址应答只用 STS1 的 ADDRF
 *   （I2C_GetFlag 只读一个寄存器，不会清 ADDR），
 *   清除 ADDR 用“读 STS1 + 读 STS2”两条访问。先关 ACK 再清 ADDR，最后发 STOP。
 * - 任何失败都要统一收尾：恢复 ACK、复位 NACK 位置、发 STOP、等总线空闲，
 *   否则下一次访问会一直卡在 BUSY。
 *
 * 并发：IMU 采样线程（优先级 8）与主线程（充电器轮询，优先级 10）共用
 * 这条总线。每次事务用互斥锁串行化，避免两条事务在总线上交错；
 * 单个事务 <1ms，等待时间有界。
 */
#include <rtthread.h>
#include "i2c1.h"
#include "n32wb43x.h"
#include "project_config.h"

/* 单次等待的迭代上限。108MHz 下约合数百微秒到毫秒级，
 * 具体数值需上板确认；这里只保证“不会永远等下去”。 */
#define I2C1_WAIT_LOOPS 100000U

static struct rt_mutex i2c1_lock;
static uint8_t i2c1_lock_ready;
static uint8_t i2c1_ready;

static void i2c1_bus_lock(void)
{
    if (i2c1_lock_ready != 0U)
    {
        (void)rt_mutex_take(&i2c1_lock, RT_WAITING_FOREVER);
    }
}

static void i2c1_bus_unlock(void)
{
    if (i2c1_lock_ready != 0U)
    {
        (void)rt_mutex_release(&i2c1_lock);
    }
}

/* 等某个事件位组合全部置位。内部会读 STS1+STS2，因此会清掉 ADDR。 */
static int i2c1_wait_event(uint32_t event)
{
    uint32_t guard = I2C1_WAIT_LOOPS;

    while (I2C_CheckEvent(I2C1, event) == ERROR)
    {
        if (--guard == 0U)
        {
            return I2C1_ERR_DATA;
        }
    }
    return I2C1_OK;
}

/* 等单个标志位。I2C_GetFlag 只读一个状态寄存器，不会误清 ADDR。 */
static int i2c1_wait_flag(uint32_t flag)
{
    uint32_t guard = I2C1_WAIT_LOOPS;

    while (I2C_GetFlag(I2C1, flag) == RESET)
    {
        if (--guard == 0U)
        {
            return I2C1_ERR_DATA;
        }
    }
    return I2C1_OK;
}

/* 等总线回到空闲。STOP 之后使用，保证下一次 START 不会看到 BUSY。 */
static int i2c1_wait_idle(void)
{
    uint32_t guard = I2C1_WAIT_LOOPS;

    while (I2C_GetFlag(I2C1, I2C_FLAG_BUSY) != RESET)
    {
        if (--guard == 0U)
        {
            return I2C1_ERR_BUSY;
        }
    }
    return I2C1_OK;
}

/* ADDR 标志需要先读 STS1 再读 STS2 才会清除。 */
static void i2c1_clear_addr_flag(void)
{
    (void)I2C1->STS1;
    (void)I2C1->STS2;
}

/* 失败统一收尾：把总线放回空闲且 ACK 恢复默认，避免下一次永久 BUSY。 */
static int i2c1_abort(int err)
{
    I2C_ConfigAck(I2C1, ENABLE);
    I2C_ConfigNackLocation(I2C1, I2C_NACK_POS_CURRENT);
    I2C_GenerateStop(I2C1, ENABLE);
    (void)i2c1_wait_idle();
    return err;
}

/* 首次 START：先等总线空闲，再产生 START 并等 EV5。 */
static int i2c1_start(void)
{
    int ret = i2c1_wait_idle();

    if (ret != I2C1_OK)
    {
        return ret;
    }

    I2C_GenerateStart(I2C1, ENABLE);
    return i2c1_wait_event(I2C_EVT_MASTER_MODE_FLAG);
}

/*
 * 重复 START：总线此时因为前一段传输仍然是 BUSY，这是正常现象，
 * 不能再调用 i2c1_start()（它等 BUSY 清零会一直超时）。
 */
static int i2c1_restart(void)
{
    I2C_GenerateStart(I2C1, ENABLE);
    return i2c1_wait_event(I2C_EVT_MASTER_MODE_FLAG);
}

static int i2c1_send_addr(uint8_t addr7, uint8_t direction)
{
    /* SDK 直接写 DAT，只改 bit0；7 位地址必须先左移。 */
    I2C_SendAddr7bit(I2C1, (uint8_t)(addr7 << 1), direction);

    if (direction == I2C_DIRECTION_SEND)
    {
        /* EV6(W)：地址已被应答，且发送寄存器为空。这个检查会清 ADDR，
         * 对发送方向没有影响（发数据本来就要先清）。 */
        return i2c1_wait_event(I2C_EVT_MASTER_TXMODE_FLAG);
    }

    /* 读方向只等 ADDRF，保留 ADDR 让 i2c1_recv_bytes() 按正确顺序处理
     * ACK / 清 ADDR / STOP。若这里用 CheckEvent(EV6)，ADDR 会被提前清掉。 */
    return i2c1_wait_flag(I2C_FLAG_ADDRF);
}

static int i2c1_send_bytes(const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; i++)
    {
        I2C_SendData(I2C1, buf[i]);
        /* EV8：TXE 置位表示上一个字节已进移位寄存器，可以继续写下一个。 */
        if (i2c1_wait_event(I2C_EVT_MASTER_DATA_SENDING) != I2C1_OK)
        {
            return I2C1_ERR_DATA;
        }
    }

    /* EV8_2：BTF 表示最后一个字节也已送出，此时发 STOP 或重复 START 才安全。 */
    return i2c1_wait_event(I2C_EVT_MASTER_DATA_SENDED);
}

/*
 * 接收数据。进入时地址阶段已完成且 ADDR 仍置位。
 * 按厂商例程分 1 字节 / 2 字节 / 3 字节以上三种尾部时序：
 *   - 1 字节：关 ACK → 清 ADDR → STOP → 等 RXDATNE → 读；
 *   - 2 字节：NACK 位置设为 NEXT → 清 ADDR → 关 ACK → 等 BYTEF → STOP → 连读 2 字节；
 *   - 3 字节以上：开 ACK → 清 ADDR → 逐字节等 EV7 → 剩 3 字节时按 BYTEF 关 ACK、
 *     发 STOP、连读 2 字节。
 */
static int i2c1_recv_bytes(uint8_t *buf, uint32_t len)
{
    uint32_t i = 0U;

    if (len == 0U)
    {
        return I2C1_ERR_PARAM;
    }

    if (len == 1U)
    {
        I2C_ConfigAck(I2C1, DISABLE);
        i2c1_clear_addr_flag();
        I2C_GenerateStop(I2C1, ENABLE);
        if (i2c1_wait_flag(I2C_FLAG_RXDATNE) != I2C1_OK)
        {
            return I2C1_ERR_DATA;
        }
        buf[0] = I2C_RecvData(I2C1);
        return I2C1_OK;
    }

    if (len == 2U)
    {
        I2C_ConfigNackLocation(I2C1, I2C_NACK_POS_NEXT);
        i2c1_clear_addr_flag();
        I2C_ConfigAck(I2C1, DISABLE);
        if (i2c1_wait_flag(I2C_FLAG_BYTEF) != I2C1_OK)
        {
            return I2C1_ERR_DATA;
        }
        I2C_GenerateStop(I2C1, ENABLE);
        buf[0] = I2C_RecvData(I2C1);
        buf[1] = I2C_RecvData(I2C1);
        return I2C1_OK;
    }

    I2C_ConfigAck(I2C1, ENABLE);
    i2c1_clear_addr_flag();

    /* 先收 len-3 个字节，剩下最后 3 个走专用尾部时序。 */
    while (i < (len - 3U))
    {
        if (i2c1_wait_event(I2C_EVT_MASTER_DATA_RECVD_FLAG) != I2C1_OK)
        {
            return I2C1_ERR_DATA;
        }
        buf[i] = I2C_RecvData(I2C1);
        i++;
    }

    /* 倒数第 3 个：等 BTF 后关 ACK，保证最后一个字节被 NACK。 */
    if (i2c1_wait_flag(I2C_FLAG_BYTEF) != I2C1_OK)
    {
        return I2C1_ERR_DATA;
    }
    I2C_ConfigAck(I2C1, DISABLE);
    buf[i] = I2C_RecvData(I2C1);
    i++;

    /* 倒数第 2 个：BTF 后发 STOP，然后一次读走剩下 2 个。 */
    if (i2c1_wait_flag(I2C_FLAG_BYTEF) != I2C1_OK)
    {
        return I2C1_ERR_DATA;
    }
    I2C_GenerateStop(I2C1, ENABLE);
    buf[i] = I2C_RecvData(I2C1);
    i++;
    buf[i] = I2C_RecvData(I2C1);

    return I2C1_OK;
}

int I2c1_Init(void)
{
    I2C_InitType init;
    GPIO_InitType gpio;

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_I2C1, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOB, ENABLE);

    /* PB8 = SCL，PB9 = SDA，复用开漏；模块自带上拉时也可以再加内部上拉。 */
    GPIO_InitStruct(&gpio);
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.GPIO_Slew_Rate = GPIO_Slew_Rate_High;
    gpio.GPIO_Mode = GPIO_Mode_AF_OD;
    gpio.GPIO_Alternate = GPIO_AF4_I2C1;
    gpio.GPIO_Pull = GPIO_Pull_Up;
    GPIO_InitPeripheral(GPIOB, &gpio);

    I2C_DeInit(I2C1);
    I2C_InitStruct(&init);
    init.BusMode = I2C_BUSMODE_I2C;
    init.FmDutyCycle = I2C_FMDUTYCYCLE_2;
    init.OwnAddr1 = 0x30U;         /* 主模式本地地址，从机不会用到 */
    init.AckEnable = I2C_ACKEN;
    init.AddrMode = I2C_ADDR_MODE_7BIT;
    init.ClkSpeed = RD91_I2C1_CLK_HZ;  /* 速率见 inc/project_config.h 说明 */
    I2C_Init(I2C1, &init);
    I2C_Enable(I2C1, ENABLE);

    /* 互斥对象只建一次；重复初始化（例如以后加入第二个器件）时保留原锁。 */
    if (i2c1_lock_ready == 0U)
    {
        if (rt_mutex_init(&i2c1_lock, "i2c1", RT_IPC_FLAG_PRIO) == RT_EOK)
        {
            i2c1_lock_ready = 1U;
        }
    }
    i2c1_ready = 1U;

    return I2C1_OK;
}

int I2c1_IsReady(void)
{
    return (i2c1_ready != 0U) ? 1 : 0;
}

/* 以下两个 *_locked 版本要求调用者已持有总线锁，不要在别处直接调用。 */
static int i2c1_write_locked(uint8_t addr7, const uint8_t *wr, uint32_t wr_len)
{
    int ret;

    if (wr == 0 || wr_len == 0U)
    {
        return I2C1_ERR_PARAM;
    }

    ret = i2c1_start();
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    ret = i2c1_send_addr(addr7, I2C_DIRECTION_SEND);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(I2C1_ERR_ADDR);
    }

    ret = i2c1_send_bytes(wr, wr_len);
    I2C_GenerateStop(I2C1, ENABLE);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    return i2c1_wait_idle();
}

static int i2c1_writeread_locked(uint8_t addr7,
                                 const uint8_t *wr, uint32_t wr_len,
                                 uint8_t *rd, uint32_t rd_len)
{
    int ret;

    if (rd == 0 || rd_len == 0U || wr == 0 || wr_len == 0U)
    {
        return I2C1_ERR_PARAM;
    }

    ret = i2c1_start();
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    ret = i2c1_send_addr(addr7, I2C_DIRECTION_SEND);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(I2C1_ERR_ADDR);
    }

    ret = i2c1_send_bytes(wr, wr_len);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    /* 重复起始：这里总线必然还是 BUSY，不能走 i2c1_start()。 */
    ret = i2c1_restart();
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    ret = i2c1_send_addr(addr7, I2C_DIRECTION_RECV);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(I2C1_ERR_ADDR);
    }

    ret = i2c1_recv_bytes(rd, rd_len);

    /* 收尾与实际完成度无关：ACK、NACK 位置、STOP、总线空闲一律恢复。 */
    I2C_ConfigAck(I2C1, ENABLE);
    I2C_ConfigNackLocation(I2C1, I2C_NACK_POS_CURRENT);
    if (ret != I2C1_OK)
    {
        return i2c1_abort(ret);
    }

    return i2c1_wait_idle();
}

/* 对外接口：加锁后执行一次完整事务，所有返回路径都会解锁。 */
int I2c1_Write(uint8_t addr7, const uint8_t *wr, uint32_t wr_len)
{
    int ret;

    i2c1_bus_lock();
    ret = i2c1_write_locked(addr7, wr, wr_len);
    i2c1_bus_unlock();
    return ret;
}

int I2c1_WriteRead(uint8_t addr7,
                   const uint8_t *wr, uint32_t wr_len,
                   uint8_t *rd, uint32_t rd_len)
{
    int ret;

    i2c1_bus_lock();
    ret = i2c1_writeread_locked(addr7, wr, wr_len, rd, rd_len);
    i2c1_bus_unlock();
    return ret;
}
