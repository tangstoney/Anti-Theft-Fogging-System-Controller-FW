# 后续开发任务顺序

## P0 当前骨架

厂家RTT3.1.4 + GCC启动/链接 + console + 灯 + PA0消抖自检 + 静态消息队列。app为普通C，RTT线程输入输出在src适配。只建立底座，不冒充产品功能。当前构建与未上板状态见verification.md。

## P1 两板BLE与布撤防

1. 记录到手板/COM/跳线，验证RTT调度、console、按键、灯和栈余量。
2. 查询M0 AT应答/INFO/MAC；按SDK master/slave把UART4和控制脚接入RTT。先核对驱动映射、IRQ/DMA唯一归属和AT超时恢复。
3. 首先controller↔keyfob两板连接/回环，再替换自动测试流与slave回环，做显式ARM/DISARM和带请求号业务回执。出厂默认撤防已确认。
4. 分别验证controller↔fogger两板模拟执行与结果，不假定三节点并发可用。

## P2 传感器与业务

IMU 两条检测链路已实现（代码完成，未上板验证）：100Hz 采样线程 + 运动状态（秒级振动 RMS 迟滞判断）+ 疑似冲击（逐样本 |a| 峰值聚合），见 app/motion_stats.c、app/shock_detect.c；参数与含义见 inc/project_config.h，验收步骤见 verification.md。本机声光报警同样代码完成、未上板：WT588F02 单线串口发码（两线代码保留，`RD91_WT_WIRE_MODE` 切换）+ F2 循环 + BUSY 确认、有源蜂鸣器、红蓝灯交替，由一个静态声光线程独占，台架入口 `Bench_AlarmSelfTest`。充电/状态指示灯代码完成、未上板：SGM41513 经共享 I2C1 读取 REG08/REG09，main 线程去抖后驱动 PA5/PA6（充电中红、充满绿），台架入口 `Bench_ChargeSimSet`。电池电量代码完成、未上板：CW2217（I2C 0x64）初始化写 CONFIG 0x30/0x00 退出 shutdown，main 线程每 2s 读 SOC/VCELL/TEMP，经 app/battery_level.c 换算与过期判断后投递 `RD91_EV_BATTERY`，台架入口 `Bench_BatterySimSet`（`Bench_ChargeSimSet`/`Bench_BatterySimSet` 默认编译关闭，置对应宏重新编译才可用）；低电阈值/告警未做（Q11）。Flash 预算：产品限制镜像 ≤50KB，`-Os` 下当前 36096B，后续两个功能块需在此预算内，新增功能前先记录尺寸增量。

下一项：上板按 IMU 六步、声光九步、充电指示灯十步与电量计十步验收、记录阈值与停止延迟；再按厂家 OUT/UART 手册接人体雷达，随后接纯 C 业务。布防/车辆/报警/执行请求拆开建模。撤防、MOVING、UNKNOWN、授权、静停入侵、超时/断线/重复ACK分别验收。识别阈值标待标定，不自行定义产品指标。

## P3 整机

供应商确认并实测连接数/角色能力后决定并发拓扑；实现已确认的鉴权、幂等、重试与持久化。显示、语音、电量逐项接入。RTT低功耗/时基恢复和OTA单独评估；不要把原厂裸机STOP2代码直接搬到线程里。

每阶段都留下构建日志、接口表、预期日志、实际结果和待确认项。发现阻塞只询问具体依赖，其他可逆工作继续。当前提示词供唐石审核，Claude执行时以其选定的阶段为范围。
