/**
 * @file imu_qmi8a01z.c
 * @brief QMI8A01z 驱动实现。只做“芯片层面”的事：配置寄存器、下发 CTRL9 参数、
 *        读三轴和状态。语义判断（这是不是砸车）留给 app/。
 *
 * 线程约束：所有 I2C 访问都在调用者线程里完成；EXTI 中断只累加计数。
 */
#include "imu_qmi8a01z.h"
#include "i2c1.h"
#include "board.h"          /* Bench_NowMs()：给 CTRL9 等待提供毫秒时基 */
#include "project_config.h"

/* 编译期检查：配置值必须落在手册允许的范围内，越界时直接编译失败，
 * 避免上板才发现参数被截断。 */
#if (RD91_IMU_ACCEL_FS_CODE > 3U)
#error "RD91_IMU_ACCEL_FS_CODE 只能是 0..3（±2/4/8/16g）"
#endif
#if (RD91_IMU_ACCEL_ODR_CODE > 8U) && (RD91_IMU_ACCEL_ODR_CODE < 12U)
#error "RD91_IMU_ACCEL_ODR_CODE 9..11 在手册中为 N/A"
#endif
#if (RD91_IMU_ANYM_AXIS_EN > 0x07U)
#error "RD91_IMU_ANYM_AXIS_EN 只有 bit0..bit2 有效"
#endif

static volatile uint32_t imu_int_count;
static uint8_t imu_ready;

/* ------------------------- 寄存器访问 ------------------------- */

/*
 * 写一个寄存器：I2C 事务是“地址 + 寄存器号 + 数据”，
 * 所以拼成 2 字节一次发出，比“写寄存器号→再写数据”少一次总线事务。
 */
static int imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = value;
    if (I2c1_Write((uint8_t)RD91_IMU_I2C_ADDR7, buf, 2U) != I2C1_OK)
    {
        return IMU_ERR_BUS;
    }
    return IMU_OK;
}

/* 读连续寄存器：先写寄存器号，再用重复起始读回 N 个字节。
 * 要求 CTRL1.ADDR_AI = 1，否则设备不会自动递增地址。 */
static int imu_read_regs(uint8_t reg, uint8_t *buf, uint32_t len)
{
    if (I2c1_WriteRead((uint8_t)RD91_IMU_I2C_ADDR7, &reg, 1U, buf, len) != I2C1_OK)
    {
        return IMU_ERR_BUS;
    }
    return IMU_OK;
}

/* ------------------------- CTRL9 命令 ------------------------- */

/*
 * 手册 §5.10（PDF p29-p32）的 CTRL9 协议：
 *   1) 主机把参数写进 CAL 寄存器；
 *   2) 主机把命令码写进 CTRL9(0x0A)；
 *   3) 设备执行完把 STATUSINT.bit7(CmdDone) 置 1；
 *   4) 主机写 CTRL9=0x00(ACK) 结束协议，设备清 CmdDone。
 *
 * 这里的握手走寄存器而不是 INT1 引脚，因为初始化时已经把
 * CTRL8.bit7 置 1（手册 §6.2，PDF p34）。否则 CTRL9 完成信号会和
 * Any-Motion 事件抢同一根 INT1，主机分不清是谁触发的中断。
 */
static int imu_ctrl9_exec(uint8_t cmd)
{
    uint32_t start_ms = Bench_NowMs();
    uint8_t status = 0U;
    int ret;

    ret = imu_write_reg(IMU_REG_CTRL9, cmd);
    if (ret != IMU_OK)
    {
        return ret;
    }

    do
    {
        if (imu_read_regs(IMU_REG_STATUSINT, &status, 1U) != IMU_OK)
        {
            return IMU_ERR_BUS;
        }
        if ((status & IMU_STATUSINT_CMD_DONE) != 0U)
        {
            break;
        }
    } while ((uint32_t)(Bench_NowMs() - start_ms) < RD91_IMU_CTRL9_TIMEOUT_MS);

    if ((status & IMU_STATUSINT_CMD_DONE) == 0U)
    {
        return IMU_ERR_CTRL9_TIMEOUT;
    }

    /* 手册要求必须回 ACK；不回的话设备一直停在 CmdDone 状态。 */
    return imu_write_reg(IMU_REG_CTRL9, IMU_CTRL9_CMD_ACK);
}

/* ------------------------- 运动检测参数 ------------------------- */

/*
 * 手册表 38（PDF p45）：运动参数分两批下发，每批都写到同样的
 * CAL1_L..CAL4_H 八个字节，用 CAL4_H 的值区分这是第几批。
 *
 * 第一批：三轴 Any-Motion 阈值、三轴 No-Motion 阈值、MOTION_MODE_CTRL。
 * 第二批：各种窗口长度（单位都是“采样个数”，不是毫秒）。
 */
static int imu_configure_motion(void)
{
    uint8_t set1[8];
    uint8_t set2[8];
    uint8_t motion_mode;
    int ret;
    uint32_t i;

    /* 三轴开关 + 轴间逻辑。bit0/1/2 恰好就是 AnyMotionEnX/Y/Z。 */
    motion_mode = (uint8_t)(RD91_IMU_ANYM_AXIS_EN & 0x07U);
    if (RD91_IMU_ANYM_AXIS_LOGIC_AND != 0U)
    {
        motion_mode |= IMU_MOTION_CTRL_ANY_AXIS_LOGIC_AND;
    }

    set1[0] = (uint8_t)RD91_IMU_ANYM_THR_X_LSB;   /* CAL1_L: AnyMotionXThr */
    set1[1] = (uint8_t)RD91_IMU_ANYM_THR_Y_LSB;   /* CAL1_H: AnyMotionYThr */
    set1[2] = (uint8_t)RD91_IMU_ANYM_THR_Z_LSB;   /* CAL2_L: AnyMotionZThr */
    set1[3] = 0U;                                  /* CAL2_H: NoMotionXThr（未启用） */
    set1[4] = 0U;                                  /* CAL3_L: NoMotionYThr（未启用） */
    set1[5] = 0U;                                  /* CAL3_H: NoMotionZThr（未启用） */
    set1[6] = motion_mode;                         /* CAL4_L: MOTION_MODE_CTRL */
    set1[7] = IMU_MOTION_PARAM_SET_1;              /* CAL4_H: 0x10 = 第一批 */

    /* 第二批：本轮只用了 AnyMotionWindow，其余写 0（对应引擎未使能）。 */
    set2[0] = (uint8_t)RD91_IMU_ANYM_WINDOW_SAMPLES; /* CAL1_L: AnyMotionWindow */
    set2[1] = 0U;                                    /* CAL1_H: NoMotionWindow */
    set2[2] = 0U;                                    /* CAL2_L: SigMotionWaitWindow[7:0] */
    set2[3] = 0U;                                    /* CAL2_H: SigMotionWaitWindow[15:8] */
    set2[4] = 0U;                                    /* CAL3_L: SigMotionConfirmWindow[7:0] */
    set2[5] = 0U;                                    /* CAL3_H: SigMotionConfirmWindow[15:8] */
    set2[6] = 0U;                                    /* CAL4_L: NA */
    set2[7] = IMU_MOTION_PARAM_SET_2;                /* CAL4_H: 0x20 = 第二批 */

    for (i = 0U; i < 8U; i++)
    {
        if (imu_write_reg((uint8_t)(IMU_REG_CAL1_L + i), set1[i]) != IMU_OK)
        {
            return IMU_ERR_BUS;
        }
    }
    ret = imu_ctrl9_exec(IMU_CTRL9_CMD_CONFIGURE_MOTION);
    if (ret != IMU_OK)
    {
        return ret;
    }

    for (i = 0U; i < 8U; i++)
    {
        if (imu_write_reg((uint8_t)(IMU_REG_CAL1_L + i), set2[i]) != IMU_OK)
        {
            return IMU_ERR_BUS;
        }
    }
    return imu_ctrl9_exec(IMU_CTRL9_CMD_CONFIGURE_MOTION);
}

/* ------------------------- 对外接口 ------------------------- */

int Imu_Init(void)
{
    uint8_t id = 0U;
    uint8_t rev = 0U;
    uint8_t ctrl1;
    int ret;

    imu_ready = 0U;
    imu_int_count = 0U;

    /* 1) 先确认总线和器件在不在。地址不对时这里就会失败，
     *    不会继续往一个不存在的器件写配置。 */
    if (imu_read_regs(IMU_REG_WHO_AM_I, &id, 1U) != IMU_OK)
    {
        return IMU_ERR_BUS;
    }
    if (id != IMU_WHO_AM_I_VALUE)
    {
        return IMU_ERR_ID;
    }
    (void)imu_read_regs(IMU_REG_REVISION_ID, &rev, 1U);

    /* 2) 配置期间必须先关闭加速度计和陀螺仪（手册表 38 下方 Note，PDF p45）。
     *    CTRL7 写 0 同时保证 SyncSample=0 —— 运动检测只在 Non-SyncSample 下工作。 */
    ret = imu_write_reg(IMU_REG_CTRL7, 0x00U);
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 3) 先选 CTRL9 握手方式：用 STATUSINT 寄存器，不占 INT1。
     *    此时所有检测引擎都还是关闭的。 */
    ret = imu_write_reg(IMU_REG_CTRL8, IMU_CTRL8_CTRL9_VIA_STATUSINT);
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 4) CTRL1：小端 + 地址自增。INT1 暂不使能，等配置全部完成再打开。 */
    ctrl1 = IMU_CTRL1_ADDR_INC;
    ret = imu_write_reg(IMU_REG_CTRL1, ctrl1);
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 5) CTRL2：量程 + ODR。两者共用一个寄存器，所以一次写。 */
    ret = imu_write_reg(IMU_REG_CTRL2,
                        (uint8_t)(IMU_CTRL2_FS(RD91_IMU_ACCEL_FS_CODE)
                                  | IMU_CTRL2_ODR(RD91_IMU_ACCEL_ODR_CODE)));
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 6) CTRL5：加速度低通滤波。 */
    if (RD91_IMU_ACCEL_LPF_EN != 0U)
    {
        ret = imu_write_reg(IMU_REG_CTRL5,
                            (uint8_t)(IMU_CTRL5_ALPF_MODE(RD91_IMU_ACCEL_LPF_MODE)
                                      | IMU_CTRL5_ALPF_EN));
        if (ret != IMU_OK)
        {
            return ret;
        }
    }

    /* 7) 下发 Any-Motion 参数（加速度计仍处于关闭状态）。 */
    ret = imu_configure_motion();
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 8) 打开加速度计。陀螺仪保持关闭（CTRL7.gEN=0），省电。 */
    ret = imu_write_reg(IMU_REG_CTRL7, IMU_CTRL7_AEN);
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 9) 打开 Any-Motion 引擎，并把运动事件指向 INT1。
     *
     *    CTRL8.bit6 ACTIVITY_INT_SEL 出厂默认 0 = 事件走 INT2；
     *    本板用杜邦线把 IMU 的 INT1 接到 MCU 的 PC0，所以必须显式改成 1。
     *    若漏掉这一位，芯片照常检测、寄存器照常有状态，但引脚永远不动，
     *    上板现象就是“一切正常、却从来收不到中断”。
     *    顺便说明：Any/No/Significant/Tap 四种引擎共用这一位，不能分开选。 */
    ret = imu_write_reg(IMU_REG_CTRL8,
                        (uint8_t)(IMU_CTRL8_CTRL9_VIA_STATUSINT
                                  | IMU_CTRL8_ACTIVITY_INT1
                                  | IMU_CTRL8_ANYMOTION_EN));
    if (ret != IMU_OK)
    {
        return ret;
    }

    /* 10) 最后才把 INT1 设成推挽输出。放在最后是为了避免配置过程中
     *     产生无意义的边沿。CTRL1 需要读改写，保留前面设置的 ADDR_INC。 */
    ctrl1 = (uint8_t)(IMU_CTRL1_ADDR_INC | IMU_CTRL1_INT1_EN);
    ret = imu_write_reg(IMU_REG_CTRL1, ctrl1);
    if (ret != IMU_OK)
    {
        return ret;
    }

    imu_ready = 1U;
    (void)rev;
    return IMU_OK;
}

/*
 * 读一帧完整样本：0x2E..0x3A 共 13 字节（STATUS0、STATUS1、样本计数 3 字节、
 * 温度 2 字节、三轴 6 字节）。
 *
 * 手册没有说明输出寄存器在读取期间会锁存（STATUSINT.bit6 Locked 只在
 * CTRL7.syncSmpl=1 时有效，而运动检测要求 syncSmpl=0）。也就是说，
 * 一次 13 字节读的过程中芯片可能正好输出新样本，读到的可能是
 * “旧计数 + 新三轴”之类的混帧。混帧对运动 RMS 影响不大，
 * 但在冲击链路上会凭空造出一个不存在的峰值，所以必须检测出来。
 *
 * 检测方法：0x30..0x32 在帧内偏移 2..4，早于三轴字节。读完这一帧后再读一次
 * 这 3 个字节：
 *   - 计数没变 → 从读计数到读三轴之间没有新样本插入，本帧一致；
 *   - 计数变了 → 期间发生过更新，三轴可能来自另一个采样点，整帧丢弃。
 * 代价是与芯片更新重叠时少一个软件样本（n 会略低于 100Hz 理论值），
 * 换来的是绝不把混帧当峰值。彻底方案是改用 FIFO（每条记录是一组完整样本），
 * 那需要另外验证 FIFO 寄存器语义，本轮不做。
 */
int Imu_ReadFrame(ImuFrame *frame)
{
    uint8_t buf[13];
    uint8_t ts[3];
    uint32_t counter_after;

    if (frame == 0)
    {
        return IMU_ERR_PARAM;
    }
    if (imu_ready == 0U)
    {
        return IMU_ERR_BUS;
    }

    if (imu_read_regs(IMU_REG_STATUS0, buf, 13U) != IMU_OK)
    {
        return IMU_ERR_BUS;
    }

    frame->status0 = buf[0];
    frame->status1 = buf[1];
    /* 芯片样本计数：每输出一个三轴样本 +1，24 位循环（0x30..0x32）。
     * 用它可以看出软件采样期间芯片实际产生了多少样本。 */
    frame->sample_counter = ((uint32_t)buf[4] << 16) |
                            ((uint32_t)buf[3] << 8) |
                            (uint32_t)buf[2];

    /* 一致性复核，见函数头注释。 */
    if (imu_read_regs(IMU_REG_TIMESTAMP, ts, 3U) != IMU_OK)
    {
        return IMU_ERR_BUS;
    }
    counter_after = ((uint32_t)ts[2] << 16) |
                    ((uint32_t)ts[1] << 8) |
                    (uint32_t)ts[0];
    if (counter_after != frame->sample_counter)
    {
        return IMU_ERR_INCONSISTENT;
    }

    /*
     * 0x35..0x3A 依次是 AX_L、AX_H、AY_L、AY_H、AZ_L、AZ_H，
     * 在整帧里偏移 7..12。CTRL1.BE 已清 0（小端），所以低字节在前。
     * 16 位补码：先拼成 uint16，再强转 int16 让符号位生效。
     */
    frame->raw.x = (int16_t)(((uint16_t)buf[8] << 8) | (uint16_t)buf[7]);
    frame->raw.y = (int16_t)(((uint16_t)buf[10] << 8) | (uint16_t)buf[9]);
    frame->raw.z = (int16_t)(((uint16_t)buf[12] << 8) | (uint16_t)buf[11]);
    return IMU_OK;
}

int Imu_ReadAccelRaw(ImuAccelRaw *raw)
{
    uint8_t buf[6];

    if (raw == 0)
    {
        return IMU_ERR_PARAM;
    }
    if (imu_ready == 0U)
    {
        return IMU_ERR_BUS;
    }

    /* 0x35..0x3A 依次是 AX_L、AX_H、AY_L、AY_H、AZ_L、AZ_H。
     * 因为 CTRL1.BE 已经被清 0（小端），收到的就是低字节在前。 */
    if (imu_read_regs(IMU_REG_AX_L, buf, 6U) != IMU_OK)
    {
        return IMU_ERR_BUS;
    }

    raw->x = (int16_t)(((uint16_t)buf[1] << 8) | (uint16_t)buf[0]);
    raw->y = (int16_t)(((uint16_t)buf[3] << 8) | (uint16_t)buf[2]);
    raw->z = (int16_t)(((uint16_t)buf[5] << 8) | (uint16_t)buf[4]);
    return IMU_OK;
}

int Imu_RawSaturated(int16_t raw)
{
    /* 触达 16 位端点说明输出已经削顶；此时峰值只能当“至少这么大”。 */
    return (raw == 32767 || raw == -32768) ? 1 : 0;
}
int Imu_ReadStatus1(uint8_t *status1)
{
    if (status1 == 0)
    {
        return IMU_ERR_PARAM;
    }
    return imu_read_regs(IMU_REG_STATUS1, status1, 1U);
}

int32_t Imu_RawToMg(int16_t raw)
{
    int32_t lsb_per_g;

    /* 手册 §3.4 灵敏度表（PDF p13）：量程与 LSB/g 的对应关系。
     * ±2g → 16384，±4g → 8192，±8g → 4096，±16g → 2048。 */
    switch (RD91_IMU_ACCEL_FS_CODE)
    {
        case 0U:  lsb_per_g = 16384; break;
        case 1U:  lsb_per_g = 8192;  break;
        case 2U:  lsb_per_g = 4096;  break;
        default:  lsb_per_g = 2048;  break;
    }

    /* raw * 1000 最大约 3.3e7，int32 足够，不会溢出。 */
    return ((int32_t)raw * 1000) / lsb_per_g;
}

void Imu_IsrNotify(void)
{
    /* 中断上下文：只做这一件事。I2C 读寄存器必须回到线程里做。 */
    imu_int_count++;
}

uint32_t Imu_IntCount(void)
{
    return imu_int_count;
}

int Imu_IsReady(void)
{
    return (imu_ready != 0U) ? 1 : 0;
}
