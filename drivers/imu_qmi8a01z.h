/**
 * @file imu_qmi8a01z.h
 * @brief QST QMI8A01z 六轴 IMU 驱动（本轮只用到加速度计 + Any-Motion）。
 *
 * 手册：D:\Tangshi\Programs\Hqvt\rd91\外挂模块\QMI8A01z Datasheet Rev A 运动传感器.pdf
 * 下文标注的页码是 PDF 页码，不是手册页脚编号。
 *
 * 重要概念（手册 §9.1，PDF p40）：
 * Any-Motion 判断的是“相邻两个采样的加速度差”|a[n] - a[n-1]| 是否超过阈值，
 * 而不是“加速度峰值的绝对值”。所以阈值 2 LSB(=0.0625g) 表示：
 * 相邻两次采样之间加速度跳变了 0.0625g 以上。它和“砸车”“冲击 1.5g”不是同一件事。
 *
 * 本芯片没有独立的 shock / high-g / freefall 检测引擎（手册 p1 Features 与
 * §9 全文可查证），只有 Tap、Any-Motion、No-Motion、Significant-Motion 四种。
 */
#ifndef RD91_IMU_QMI8A01Z_H
#define RD91_IMU_QMI8A01Z_H

#include <stdint.h>

/* ==================== 返回值 ==================== */
#define IMU_OK                   0
#define IMU_ERR_BUS             (-1)  /* I2C 传输失败 */
#define IMU_ERR_ID              (-2)  /* WHO_AM_I 不匹配 */
#define IMU_ERR_CTRL9_TIMEOUT   (-3)  /* CTRL9 命令未在超时内完成 */
#define IMU_ERR_PARAM           (-4)
#define IMU_ERR_INCONSISTENT    (-5)  /* 读取期间芯片更新了样本，本帧已丢弃 */

/* ==================== 寄存器地址 ====================
 * 来源：手册表 20 UI Register Overview（PDF p19-p21）。
 */
#define IMU_REG_WHO_AM_I      0x00U
#define IMU_REG_REVISION_ID   0x01U
#define IMU_REG_CTRL1         0x02U
#define IMU_REG_CTRL2         0x03U
#define IMU_REG_CTRL3         0x04U
#define IMU_REG_CTRL5         0x06U
#define IMU_REG_CTRL7         0x08U
#define IMU_REG_CTRL8         0x09U
#define IMU_REG_CTRL9         0x0AU
/* 主机把 CTRL9 命令的参数先写进 CAL1_L..CAL4_H（手册表 33，PDF p31）。 */
#define IMU_REG_CAL1_L        0x0BU
#define IMU_REG_CAL1_H        0x0CU
#define IMU_REG_CAL2_L        0x0DU
#define IMU_REG_CAL2_H        0x0EU
#define IMU_REG_CAL3_L        0x0FU
#define IMU_REG_CAL3_H        0x10U
#define IMU_REG_CAL4_L        0x11U
#define IMU_REG_CAL4_H        0x12U
#define IMU_REG_STATUSINT     0x2DU  /* bit7 = CTRL9 CmdDone（手册表 28，p26） */
#define IMU_REG_STATUS0       0x2EU
#define IMU_REG_STATUS1       0x2FU  /* bit7/6/5 = Sig/No/Any-Motion（p27） */
/* 3 字节样本时间戳（0x30..0x32）：每输出一组 x/y/z 样本 +1，24 位循环。
 * 手册表 28 只说明“计数随样本递增”，没有说明输出寄存器会锁存，
 * 所以本驱动用它做一帧读取的一致性复核，见 Imu_ReadFrame()。 */
#define IMU_REG_TIMESTAMP     0x30U
#define IMU_REG_AX_L          0x35U  /* 加速度三轴，AX/AY/AZ 各 2 字节，p27-p28 */
#define IMU_REG_AX_H          0x36U
#define IMU_REG_AY_L          0x37U
#define IMU_REG_AY_H          0x38U
#define IMU_REG_AZ_L          0x39U
#define IMU_REG_AZ_H          0x3AU

/* ==================== 位定义 ==================== */

/* CTRL1：串行接口与传感器使能（手册表 23，PDF p22）。
 * 注意 BE(bit5) 出厂默认是 1（大端读数据）；本驱动要按 AX_L 在低地址的
 * 顺序拼 16 位数据，所以必须显式清 0。 */
#define IMU_CTRL1_SIM_3WIRE   (1U << 7)
#define IMU_CTRL1_ADDR_INC    (1U << 6)  /* 1 = 多字节读时地址自增，突发读需要 */
#define IMU_CTRL1_BE_BIG      (1U << 5)  /* 1 = 大端（默认），0 = 小端 */
#define IMU_CTRL1_INT2_EN     (1U << 4)  /* 1 = INT2 推挽输出，否则高阻 */
#define IMU_CTRL1_INT1_EN     (1U << 3)  /* 1 = INT1 推挽输出，否则高阻 */
#define IMU_CTRL1_FIFO_INT1   (1U << 2)

/* CTRL2：加速度量程与 ODR（手册表 23，p22-p23）。 */
#define IMU_CTRL2_FS(code)    ((uint8_t)(((uint8_t)(code) & 0x07U) << 4))
#define IMU_CTRL2_ODR(code)   ((uint8_t)((uint8_t)(code) & 0x0FU))

/* CTRL5：低通滤波（手册表 25，PDF p24）。
 * bit2:1 = 模式（带宽占 ODR 的比例），bit0 = 使能。 */
#define IMU_CTRL5_ALPF_MODE(mode) ((uint8_t)(((uint8_t)(mode) & 0x03U) << 1))
#define IMU_CTRL5_ALPF_EN         (1U << 0)

/* CTRL7：使能传感器（手册表 26，PDF p24-p25）。
 * SyncSample 必须为 0：手册 §9 说明运动检测只能在 Non-SyncSample 模式下工作。 */
#define IMU_CTRL7_SYNC_SAMPLE (1U << 7)
#define IMU_CTRL7_DRDY_DIS    (1U << 5)
#define IMU_CTRL7_GEN         (1U << 1)  /* 陀螺仪使能；本轮保持 0 省电 */
#define IMU_CTRL7_AEN         (1U << 0)  /* 加速度计使能 */

/* CTRL8：运动检测控制（手册表 23 续，PDF p25）。 */
#define IMU_CTRL8_CTRL9_VIA_STATUSINT (1U << 7)  /* 1 = CTRL9 握手用寄存器，不占用 INT1 */
#define IMU_CTRL8_ACTIVITY_INT1       (1U << 6)  /* 1 = 运动/Tap 事件走 INT1，0 = 走 INT2 */
#define IMU_CTRL8_SIGMOTION_EN        (1U << 3)
#define IMU_CTRL8_NOMOTION_EN         (1U << 2)
#define IMU_CTRL8_ANYMOTION_EN        (1U << 1)
#define IMU_CTRL8_TAP_EN              (1U << 0)

/* STATUS1：芯片检测到的运动事件状态（手册表 28，PDF p27）。
 * 手册原文只写 "Any-Motion was detected"，**没有**说明读取是否会清除这些位、
 * 是否会撤下 INT。本驱动不依赖这一点：唤醒只靠 EXTI 边沿计数，
 * STATUS1 仅作为日志里的定位信息。 */
#define IMU_STATUS1_SIGMOTION (1U << 7)
#define IMU_STATUS1_NOMOTION  (1U << 6)
#define IMU_STATUS1_ANYMOTION (1U << 5)
#define IMU_STATUS1_TAP       (1U << 1)

/* STATUS0：输出数据状态（手册表 28，PDF p27）。
 * bit0 = aDA，加速度计“上次读取后是否有新数据”。
 * 每次采样先确认这一位，避免把同一份旧数据重复送进两条检测链路。 */
#define IMU_STATUS0_ADATA     (1U << 0)

/* STATUSINT：CTRL9 命令完成标志（手册表 28，PDF p26）。 */
#define IMU_STATUSINT_CMD_DONE (1U << 7)

/* CTRL9 命令码（手册表 32，PDF p30-p31）。 */
#define IMU_CTRL9_CMD_ACK             0x00U
#define IMU_CTRL9_CMD_CONFIGURE_MOTION 0x0EU

/* MOTION_MODE_CTRL 字节（手册表 37，PDF p45）。
 * 注意：AnyMotionEnX/Y/Z 正好落在 bit0/1/2，与工程配置里
 * RD91_IMU_ANYM_AXIS_EN 的 bit 定义一致，可以直接搬。 */
#define IMU_MOTION_CTRL_NO_AXIS_LOGIC_AND  (1U << 7)
#define IMU_MOTION_CTRL_NO_EN_Z            (1U << 6)
#define IMU_MOTION_CTRL_NO_EN_Y            (1U << 5)
#define IMU_MOTION_CTRL_NO_EN_X            (1U << 4)
#define IMU_MOTION_CTRL_ANY_AXIS_LOGIC_AND (1U << 3)
#define IMU_MOTION_CTRL_ANY_EN_Z           (1U << 2)
#define IMU_MOTION_CTRL_ANY_EN_Y           (1U << 1)
#define IMU_MOTION_CTRL_ANY_EN_X           (1U << 0)

/* 手册表 38（PDF p45）：两批参数的 CAL4_H 必须写不同的标志值。
 * 表里写的是 CAL4_H(7:4)=0x01 / 0x02，所以字节值分别是 0x10 / 0x20。 */
#define IMU_MOTION_PARAM_SET_1  0x10U
#define IMU_MOTION_PARAM_SET_2  0x20U

/* WHO_AM_I 的固定值（手册表 22，PDF p22）。 */
#define IMU_WHO_AM_I_VALUE   0x05U

/* ==================== 数据结构 ==================== */

/* 三轴原始补码值，单位 LSB。换算成 mg 用 Imu_RawToMg()。 */
typedef struct
{
    int16_t x;
    int16_t y;
    int16_t z;
} ImuAccelRaw;

/*
 * 一次读到的完整样本帧：状态 + 芯片样本计数 + 三轴。
 * 0x2E..0x3A 在寄存器表里连续，所以用一次突发读完成，
 * 保证状态、样本计数和三轴来自同一个采样点，不会读到半新半旧的数据。
 */
typedef struct
{
    uint8_t status0;         /* bit0 = aDA（上次读取后是否有新数据） */
    uint8_t status1;         /* 芯片运动事件状态位，仅用于日志 */
    uint32_t sample_counter; /* 芯片样本计数：随采样更新递增，24 位循环 */
    ImuAccelRaw raw;
} ImuFrame;

/* ==================== 接口 ==================== */

/*
 * 初始化的前置条件：I2c1_Init() 已成功、Board_ImuIntInit() 已调用。
 * 返回 IMU_OK 或负的错误码。
 */
int Imu_Init(void);

/* 读一帧 0x2E..0x3A（13 字节）。调用者用 status0 的 aDA 判断是否是新样本；
 * 返回 IMU_ERR_INCONSISTENT 表示读取期间芯片恰好更新，本帧已丢弃（不是错误）。 */
int Imu_ReadFrame(ImuFrame *frame);

/* 读三轴加速度（一次突发读 0x35..0x3A）。 */
int Imu_ReadAccelRaw(ImuAccelRaw *raw);

/* 读 STATUS1，返回芯片当前事件状态位。 */
int Imu_ReadStatus1(uint8_t *status1);

/* 把原始值按当前量程换算成 mg（1g = 1000mg）。 */
int32_t Imu_RawToMg(int16_t raw);

/* 1 = 该轴读数已触达 16 位量程端点，说明峰值被截断，不能当作真实值。 */
int Imu_RawSaturated(int16_t raw);

/* EXTI 中断里调用：只累加计数，不做 I2C、不调用任何 RTT 接口。 */
void Imu_IsrNotify(void);

/* 读取中断计数。主线程用它判断“自上次处理以来是否发生过中断边沿”。 */
uint32_t Imu_IntCount(void);

/* 初始化是否成功（1=成功）。 */
int Imu_IsReady(void);

#endif /* RD91_IMU_QMI8A01Z_H */
