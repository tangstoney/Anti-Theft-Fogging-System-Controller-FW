/**
 * @file sgm41513.h
 * @brief SGM41513 充电器状态读取（I2C，地址 0x1A）。
 *
 * 资料：SGM41513/SGM41513A/SGM41513D 数据手册（2025-04），
 *       REGISTER MAPS（REG08/REG09）与“I2C 故障读取需连续读两次”的说明。
 *
 * 只做“读状态”，不写任何寄存器：
 *   - 不设置 WD_RST，因此充电器保持上电默认的自主充电模式（default mode），
 *     MCU 不在时也能正常充电；
 *   - 默认看门狗 160s 到期会置 WATCHDOG_FAULT 并恢复默认寄存器，
 *     这是默认模式的自有行为，不当作充电故障（本驱动不暴露该位）。
 *
 * 调用约定：使用 I2C1 总线，函数内部由 bsp/i2c1.c 的互斥锁串行化；
 * 可在任意线程调用，但不要放在中断里。读失败返回负值，寄存器内容无效。
 */
#ifndef RD91_SGM41513_H
#define RD91_SGM41513_H

#include <stdint.h>

#define SGM41513_ADDR7      0x1AU

#define SGM_OK              0
#define SGM_ERR_BUS        (-1)  /* I2C 传输失败或从机未应答 */
#define SGM_ERR_PARAM      (-2)

/* REG08 状态位 */
#define SGM_REG08_VBUS_STAT(reg)  ((uint8_t)(((reg) >> 5) & 0x07U))
#define SGM_REG08_CHRG_STAT(reg)  ((uint8_t)(((reg) >> 3) & 0x03U))
#define SGM_REG08_PG_STAT(reg)    ((uint8_t)(((reg) >> 2) & 0x01U))
#define SGM_REG08_THERM_STAT(reg) ((uint8_t)(((reg) >> 1) & 0x01U))
#define SGM_REG08_VSYS_STAT(reg)  ((uint8_t)((reg) & 0x01U))

/* REG09 故障位 */
#define SGM_REG09_WDT_FAULT(reg)  ((uint8_t)(((reg) >> 7) & 0x01U))
#define SGM_REG09_BOOST_FAULT(reg) ((uint8_t)(((reg) >> 6) & 0x01U))
#define SGM_REG09_CHRG_FAULT(reg) ((uint8_t)(((reg) >> 4) & 0x03U))
#define SGM_REG09_BAT_FAULT(reg)  ((uint8_t)(((reg) >> 3) & 0x01U))
#define SGM_REG09_NTC_FAULT(reg)  ((uint8_t)((reg) & 0x07U))

typedef struct
{
    uint8_t reg08;
    uint8_t reg09;
    uint8_t vbus_stat;
    uint8_t chrg_stat;  /* 00 禁用 / 01 预充 / 10 快充 / 11 充满 */
    uint8_t pg_stat;    /* 1 = 输入电源正常 */
    uint8_t therm_stat; /* 1 = 正在做热调节 */
    uint8_t vsys_stat;  /* 1 = 系统电压跌到 VSYS_MIN 以下 */
    uint8_t chrg_fault; /* 00 正常 / 01 输入故障 / 10 过温 / 11 安全定时器超时 */
    uint8_t bat_fault;  /* 1 = 电池过压 */
    uint8_t ntc_fault;  /* 000 正常 / 010 暖 / 011 凉 / 101 冷 / 110 热 */
} Sgm41513Status;

/* 读取 REG0B 的 PN[3:0] 与 DEV_REV[1:0]，用于确认器件在位。 */
int Sgm41513_ReadId(uint8_t *pn, uint8_t *dev_rev);

/*
 * 读取当前状态。
 * REG09 先读一次清除历史故障，再读一次取“当前故障”；REG08 读一次。
 * 返回 SGM_OK 时 out 有效；否则 out 内容无意义。
 */
int Sgm41513_ReadStatus(Sgm41513Status *out);

#endif /* RD91_SGM41513_H */
