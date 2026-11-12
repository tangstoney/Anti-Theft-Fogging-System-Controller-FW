/**
 * @file i2c1.h
 * @brief I2C1 主机总线（PB8=SCL / PB9=SDA，AF4），阻塞式轮询实现。
 *
 * 只提供“写”和“先写后读”两种事务，寄存器语义留给各器件驱动。
 * 所有函数都有超时，不会无限等待总线。
 *
 * 并发：IMU 采样线程与主线程（充电器轮询）共用这条总线，
 * 每次事务用内部互斥锁串行化；函数可在任意线程调用，禁止在中断里调用。
 */
#ifndef RD91_I2C1_H
#define RD91_I2C1_H

#include <stdint.h>

/* 返回 0 表示成功，负值表示失败（具体负值含义仅供日志定位）。 */
#define I2C1_OK            0
#define I2C1_ERR_BUSY     (-1)  /* 总线忙，或 START 未产生 */
#define I2C1_ERR_ADDR     (-2)  /* 从机地址未被应答 */
#define I2C1_ERR_DATA     (-3)  /* 数据阶段超时 */
#define I2C1_ERR_PARAM    (-4)

/* 初始化 PB8/PB9 与 I2C1，速率见 RD91_I2C1_CLK_HZ。返回 I2C1_OK 或负值。 */
int I2c1_Init(void);

/* 1 = 总线已初始化。未初始化时不得发起事务。 */
int I2c1_IsReady(void);

/* 仅写：START + 地址(W) + wr[] + STOP。 */
int I2c1_Write(uint8_t addr7, const uint8_t *wr, uint32_t wr_len);

/*
 * 先写后读（重复起始）：
 *   START + 地址(W) + wr[] + 重复START + 地址(R) + 读 rd[] + STOP
 * 这是读寄存器最常用的时序：wr 放 1 字节寄存器地址，rd 收数据。
 */
int I2c1_WriteRead(uint8_t addr7,
                   const uint8_t *wr, uint32_t wr_len,
                   uint8_t *rd, uint32_t rd_len);

#endif /* RD91_I2C1_H */
