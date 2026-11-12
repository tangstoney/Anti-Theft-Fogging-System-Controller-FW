/**
 * @file cw2217.h
 * @brief CW2217EAAD 电量计（I2C，地址 0x64）：唤醒、版本确认、SOC/电压/温度读取。
 *
 * 资料：CW2217EAAD-DS V1.0（2024，Cellwise），Register Map 与 Power States 两节。
 *   - 上电默认处于 shutdown 模式，必须向 CONFIG(0x08) 依次写 0x30、0x00 才进入
 *     active 模式（这是本驱动唯一的写操作，序列来自手册，不是自创）；
 *   - VERSION(0x00) 固定 0xA0，shutdown/active 两种模式都可读，用作在位确认；
 *   - SOC(0x04/0x05) 为无符号 16 位，单位 %：高字节是整数部分，低字节 1/256%；
 *   - VCELL(0x02/0x03) 为无符号 14 位，1 LSB = 312.5µV；
 *   - TEMP(0x06) 1 LSB = 0.5°C，偏移 -40°C。
 *
 * 不读电流（CURRENT/AVE_CUR）：换算依赖采样电阻与标定，当前硬件未确认，不猜测。
 * 每次只按单个寄存器地址读取，不依赖“读操作自动递增地址”的行为。
 *
 * 调用约定：使用 I2C1 总线，由 bsp/i2c1.c 的互斥锁串行化；禁止在中断里调用。
 */
#ifndef RD91_CW2217_H
#define RD91_CW2217_H

#include <stdint.h>

#define CW2217_ADDR7            0x64U
#define CW2217_VERSION_EXPECTED 0xA0U

#define CW2217_OK          0
#define CW2217_ERR_BUS  (-1)  /* I2C 传输失败或从机未应答 */
#define CW2217_ERR_PARAM (-2)
#define CW2217_ERR_ID   (-3)  /* VERSION 不等于 0xA0 */

/* 一次读取的原始寄存器值。换算在 app/battery_level.c 中完成。 */
typedef struct
{
    uint8_t version;    /* REG00 */
    uint16_t soc_raw;   /* REG04 << 8 | REG05 */
    uint16_t vcell_raw; /* REG02 << 8 | REG03（14 位有效） */
    uint8_t temp_raw;   /* REG06 */
} Cw2217Sample;

/* 读 VERSION 并确认等于 0xA0。返回 CW2217_OK 时 *version 有效。 */
int Cw2217_Probe(uint8_t *version);

/* 写 CONFIG：0x30 → 0x00，芯片刚上电默认处于 shutdown，需要依次执行 退出 shutdown、重启电量计算
 * 返回 CW2217_OK 或负值。 */
int Cw2217_Wake(void);

/*
 * 依次读取 VERSION、SOC、VCELL、TEMP（每个寄存器一次独立事务）。
 * SOC/VCELL 的高低字节是两次事务，跨芯片更新时可能相差 1 个 LSB，
 * 对 % 显示与 mV 日志没有实际影响。
 */
int Cw2217_ReadSample(Cw2217Sample *out);

#endif /* RD91_CW2217_H */
