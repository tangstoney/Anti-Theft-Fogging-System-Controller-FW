# 中控模块设计

负责车辆状态、授权上下文、布撤防、报警决策、显示状态与模块交互。后续将状态拆成独立维度：arm_state、vehicle_state（UNKNOWN/STILL/MOVING）、alarm_state、fog_request_state、link/auth状态；不要将布防与静停塞进同一个枚举，造成判断遗漏。

vehicle_state 当前由 IMU 的秒级振动统计推断（去均值三轴 RMS + 迟滞 + 连续窗口确认），见 app/motion_stats.c。它是“停车/行驶”的推断，不是测速：UNKNOWN/FAULT 不等于停车，启动、传感器故障、窗口样本不足都不能当作 STILL。疑似冲击（app/shock_detect.c）是独立事实，经 `RD91_EV_SHOCK` 上报，**不能直接修改 vehicle_state**，只作为后续砸车/碰撞判定的输入。

P0 仅RTT启动、日志、灯、按键自检；没有布防/撤防业务状态。首次业务迭代采用明确标记的默认撤防，车辆状态 UNKNOWN。模拟 STILL 只供台架，不得将启动或传感器故障等价为 STILL。

本机声光报警已接成独立输出服务：业务层只通过 `alarm_output` 写三路期望状态（语音/蜂鸣器/红蓝灯），由唯一的静态声光线程收敛执行，见 src/alarm_output.c 与 drivers/wt588f02.c。启动幂等、重复停止幂等，快速 start/stop 以最新期望为准；RTT tick 的 10ms 只用于闪烁与 BUSY 轮询，语音位时序由 DWT 微秒延时产生。业务报警状态机尚未启用时，台架自检入口 `Bench_AlarmSelfTest(1/0)`（PA0 或调试器，`RD91_BENCH_ALARM_SELFTEST` 控制）走同一 sink，不修改业务状态。未来无线订阅应使用 `AlarmOut_SetNotify` 与快照中的版本号，且每个订阅者各自保存待发送记录。

充电/状态指示灯是独立的本地状态显示，不是业务报警：main 线程每 `RD91_CHG_POLL_MS` 读一次 SGM41513（I2C 0x1A）的 REG08/REG09，经 app/charge_indicator.c 解码、去抖后驱动 PA5/PA6（充电中红、充满绿、故障红闪、其余灭）。状态事实来自充电器寄存器，不等于 CW2217 电量，也不参与布撤防/报警判定；读取失败只能得到 UNKNOWN，不得解释成“无输入”或“充满”。充电器只读不写，保持默认自主充电模式。台架可用 `Bench_ChargeSimSet()` 模拟状态位（默认编译关闭，置 `RD91_BENCH_CHG_SELFTEST` 重新编译），走同一条判定与点灯路径。

电池电量走独立数据源：main 线程每 `RD91_BATT_POLL_MS` 读 CW2217（I2C 0x64）的 SOC/VCELL/TEMP，app/battery_level.c 负责换算与过期判断，src/battery_gauge.c 生成 `RD91_EV_BATTERY`（`RD91_BATTERY_LOCAL`）投回事件队列，业务状态仍只由 `Rd91App` 修改。读取失败保留最近有效值、超过 `RD91_BATT_STALE_MS` 才标未知；未知不得当 0%。低电阈值与告警动作未定义（Q11），本模块不擅自设阈值。初始化向 CONFIG 写 0x30/0x00 是手册要求、全工程唯一对电量计的写操作；运行期只读。台架可用 `Bench_BatterySimSet()` 模拟百分比（默认编译关闭，置 `RD91_BENCH_BATT_SELFTEST` 重新编译）。电量的“是否充电”由充电器状态给出（未知时 0xFF），不是电量计判断。

第一条业务链：Key 的 ARM/DISARM → 中控按目标状态处理 → 返回请求号与最终状态。下一步接 IMU/雷达事件和授权结果；单个 trigger 按键不代表已完成多传感器识别。撤防仍可处理通信、查询、故障与明确的模拟车辆状态更新，不能把整个事件循环关掉。

输出执行前检查授权/布防/静停/请求有效期等已确认条件；从决定到发送之间重新核对条件。MOVING 或 UNKNOWN 不产生新的执行请求。撤防、进入行驶或取消报警与“待发送/已发送/已执行”的交互写入待确认表。

输入驱动任务：QMI8A01z I2C ID/数据/中断；觅感雷达 OUT/UART；CW2217 I2C电量；SGM41513配置/状态；WT588F02私有控制；段码LCD实际控制器/玻璃参数。逐个确认原理图/手册后实现，不根据旧框图猜寄存器或接口。双IMU相同地址时先检查SA0或总线划分。

验收优先级：布撤防回执 → 撤防不报警 → MOVING 不报警 → UNKNOWN 不执行 → 授权不误报 → 静停非法入侵模拟 → 超时/断线不算成功。彩屏、4G、继电器和OTA后续单独设计。


线程约束：本模块状态由main业务线程单点修改；input/imu/后续BLE线程通过有界队列传事件。沿用厂家RTT3.1.4，外设/AT等待不得阻塞整套业务。IMU 的 I2C 访问全部在 imu 采样线程，中断里只累加计数。声光三路由声光线程独占，业务线程不得直接操作这些 GPIO；台架灯 PB5 与报警灯分属不同线程。充电指示灯由 main 线程独占；I2C1 由 imu 与 main 两个线程共用，事务级互斥锁在 bsp/i2c1.c，禁止在中断里做 I2C。
