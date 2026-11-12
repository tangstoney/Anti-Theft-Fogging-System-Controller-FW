/**
 * @file wt588f02.h
 * @brief WT588F02-8S-C 语音芯片驱动（两线串口，GPIO + DWT 微秒时序）。
 *
 * 资料：WT588F02-8S-C 芯片说明书 V1.03，§6.2 两线串口通讯、§6.2.8 时序图。
 *
 * 约束：
 *   - 只能在声光线程调用：CLK/DATA/BUSY 三根线由该线程独占；
 *   - 发码是开环的：函数返回 OK 只表示“命令已按时序完整发出”，
 *     不代表芯片已开始播放。是否在播放只能用 BUSY 判断（Wt_BusyLevel）；
 *   - 一线上电时 CLK（IO2）不得被外部上拉，本驱动初始化后保持低电平。
 */
#ifndef RD91_WT588F02_H
#define RD91_WT588F02_H

#include <stdint.h>

#define WT_OK           0
#define WT_ERR_INIT   (-1)  /* DWT 周期计数器不可用，无法保证位时序 */
#define WT_ERR_PARAM  (-2)  /* 段号/音量超出当前单/双字节格式范围 */
#define WT_ERR_BUSY   (-3)  /* 等待 BUSY 超时，未观察到播放 */

/*
 * 使能 DWT 周期计数器（微秒延时用）。不等待芯片上电初始化。
 * 必须在其他 Wt_* 函数之前调用一次。返回 WT_OK 或 WT_ERR_INIT。
 */
int Wt_ClockInit(void);

/*
 * 等待芯片上电初始化（RD91_WT_INIT_MS），并按配置发送一次音量命令。
 * 在声光线程启动时调用一次。期间会让出 CPU。
 */
int Wt_Init(void);

/* 播放第 segment 段（一条完整命令，单/双字节由配置决定）。 */
int Wt_SendPlay(uint16_t segment);

/* F2：循环播放当前段。资料要求先发播放命令，再发循环命令。 */
int Wt_SendLoop(void);

/* FE：停止播放当前语音。 */
int Wt_SendStop(void);

/* 音量命令，level 0..15（0 最小，15 最大）。 */
int Wt_SendVolume(uint8_t level);

/* BUSY 电平：1 = 高（不播放），0 = 低（播放中）。BUSY 未接入时无意义。 */
int Wt_BusyLevel(void);

/* DWT 时间戳（µs）。约 39.8s 回绕，只能做无符号差值，不能直接比较大小。 */
uint32_t Wt_NowUs(void);

#endif /* RD91_WT588F02_H */
