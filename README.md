# rd91-controller-fw — 中控

N32WB436RFQ7 / 厂家RT-Thread3.1.4 / Arm GCC。当前为P0台架骨架，后续由Claude Code继续开发。先审核 [.ai/agent-prompt.md](.ai/agent-prompt.md)；设计与来源见 [.ai/README.md](.ai/README.md)。

## 当前内容

RTT启动、main/input/imu/声光四个线程、静态消息队列、USART3日志、PB5灯、PA0消抖按键。app/为不依赖RTT的普通C；线程与时间、日志适配位于src与bsp。

传感器：QMI8A01z 已接入 I2C1，驱动位于 [drivers/](drivers/)、总线访问位于 [bsp/i2c1.c](bsp/i2c1.c)。IMU 采样线程以 100Hz 连续读取新样本，同时驱动两条互不影响的链路：

- 运动状态（停车/行驶）：[app/motion_stats.c](app/motion_stats.c)，对每 1 秒窗口计算去均值三轴 RMS，两个阈值加连续窗口确认，输出 UNKNOWN/STILL/MOVING；
- 疑似冲击：[app/shock_detect.c](app/shock_detect.c)，逐样本比较三轴合矢量 |a|，聚合成事件并记录峰值、时长、饱和标志，只输出“疑似冲击”，不进入报警/喷烟。

芯片的 Any-Motion 中断仍然保留，只作为“发生扰动”的唤醒信号，与冲击峰值、车辆运动判断是三种不同的事实。充电/状态指示灯与电池电量已接入（SGM41513、CW2217 均经 I2C1，见下文）。当前没有 STOP2 深睡，也没有 BLE、喷烟执行输出或布撤防业务；本机声光报警已实现（含台架自检入口，见下文）。出厂默认撤防已写入需求。

## 构建

```powershell
cd "D:\Tangshi\Project\N32Demo\Anti-Theft Fogging System\rd91-controller-fw"
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
# 全量重编译：
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean
```

默认工具链为用户目录.n32studio/Toolchain；SDK为../../_sdk/Nsingtech.N32WB43x_Library.2.2.0，保持只读。可用-SdkRoot/-GccPath/-MakePath覆盖；SDK与工具链路径采用无空格路径，本仓库父目录可带空格。VS Code提供构建任务。

优化等级为 `-Os`（Makefile `OPT`）。产品约束 Flash ≤ 50KB，当前镜像 36080B，剩约 14KB 给后续两个功能块；改回 `-Og` 会多占约 4.3KB，只在需要逐行调试时临时使用，交付前改回 `-Os` 并重新记录尺寸。台架模拟入口（`Bench_*SimSet`）默认编译关闭，详见各节。

Linux CI可使用同版本SDK/Arm GNU执行make SDK_ROOT=/opt/n32-sdk；本次未运行Linux或配置Runner。

产物build/rd91_controller.elf、.hex、.bin、.map。M4向量表0x08000000，不含M0镜像/Bootloader/OTA。实际构建结果见build-summary.json和logs/build.log，硬件未验证。

## 台架接线与输出

日志USART3：PB10 TX(AF0)、PB11 RX(AF5)，115200 8N1；当前只输出日志。USB-TTL RX接PB10并共地。参考板J3常接USART1，当前需外接USB-TTL或按board.md改console。

LED PB5（高有效暂定）、按键PA0对GND/内部上拉。RTT tick=100Hz，输入线程每10ms采样，30ms消抖；每次稳定按下只打印一次BENCH_KEY并翻转台架灯。BOOT注明BENCH_STUB_IO、BLE未实现、sensors=IMU_ANYMOTION sample=100Hz charge=LED_RG_I2C batt=CW2217_I2C，每秒HEARTBEAT。

IMU 相关日志（参数与含义见 [inc/project_config.h](inc/project_config.h)）：

```text
[controller] IMU_MOTION_STAT t=12340 n=99 rms=17mg enter=0 exit=3 state=STILL chip_n=998/1000 drop=0 err=0 edges=4
[controller] t=13500 VEHICLE_STATE v1=2 v2=0
[controller] SHOCK t=18220 peak_norm=2463mg dur=41ms over=3 sat=0
[controller] IMU_MOTION_EVENT v1=1 v2=0x20
```

`IMU_MOTION_STAT` 每秒一行：`n` 参与算法的样本数，`rms` 去均值振动 RMS，`enter/exit` 迟滞确认计数，`chip_n/1000` 芯片样本计数增量与理论值对照，`drop` 丢弃帧数（无新数据或读取期间芯片更新），`err` I2C 错误次数，`edges` 累计中断边沿。`VEHICLE_STATE` 只在状态变化时出现（0=UNKNOWN 1=STILL 2=MOVING 3=FAULT）。`SHOCK` 每次独立冲击一行。

IMU QMI8A01z：SCL=PB8、SDA=PB9（I2C1 AF4，400kHz 配置，可用 `RD91_I2C1_CLK_HZ` 降回 100kHz），INT1=PC0（EXTI0 上升沿，内部下拉），3.3V 共地。模块 3.3V 供电，杜邦线尽量短；I2C 上拉由模块提供或外接 4.7k。地址默认 0x6A（SA0 高/悬空），SA0 拉低为 0x6B，改 [inc/project_config.h](inc/project_config.h) 的 `RD91_IMU_I2C_ADDR7`。上板验收步骤见 [.ai/verification.md](.ai/verification.md) 的“IMU 两条检测链路”一节。

## 本机声光报警（台架）

报警开始后：语音芯片循环播放报警段、有源蜂鸣器常响、红蓝灯每 250ms 交替；报警结束三路全部关闭。业务层调用 `alarm_output` 只登记“期望状态”，由唯一的静态声光线程收敛执行，见 [src/alarm_output.c](src/alarm_output.c)、[drivers/wt588f02.c](drivers/wt588f02.c)。

### 接线（开发板杜邦线，实板必须核对）

| 信号 | MCU 脚 | 方向 | 空闲/有效 | 说明 |
|---|---|---|---|---|
| WT588F02 DATA（IO2，单线） | PB0 | 输出 | 空闲低；每字节前高 5ms → 低 5ms；位宽见配置 | **单线数据输入在 IO2**，与两线 CLK 同一脚；建议串 1k。**IO2 严禁外部上拉**：上电为高可能让芯片进入非工作模式 |
| WT588F02 IO3（两线 DATA） | PB1 | 输出 | 单线模式不使用，保持低 | 仅两线模式作 DATA；单线接线时可不接 |
| WT588F02 BUSY（IO1） | PA1 | 输入 | 播放时低，空闲高 | MCU 内部上拉；模块未接时读为高 → BUSY 超时报故障，不会误报“正在播放” |
| 蜂鸣器驱动 | PA2 | 输出 | 高 = 导通（`RD91_BUZZER_ACTIVE_HIGH`） | 经外部三极管/MOS 驱动级；有源蜂鸣器 3~36V、≤35mA，GPIO 不直接带负载 |
| 红灯 | PA3 | 输出 | 见 `RD91_LAMP_WIRING` | 默认 3 脚共阴、高有效 |
| 蓝灯 | PA4 | 输出 | 见 `RD91_LAMP_WIRING` | 默认 3 脚共阴、高有效 |

选脚避开了 PB5（台架灯）、PA0（按键）、PB8/PB9（I2C1）、PC0（IMU INT）、PB10/PB11（console）、PA13/PA14（SWD）、PB14/PB15（未来 UART4）和 PC6~PC9（未来 M0 控制）。台架灯 PB5 仍由 main 线程按键翻转，与报警灯不是同一盏灯，不存在两个线程驱动同一输出。供电：WT 若按 5V 供电，VIH=0.8VCC=4.0V，3.3V 控制信号不达高电平门限——**要么 WT 也用 3.3V 供电，要么加电平转换**；BUSY 回读电平同理。

### 语音芯片配置（[inc/project_config.h](inc/project_config.h)）

| 宏 | 当前值 | 含义 |
|---|---|---|
| `RD91_WT_WIRE_MODE` | 1 | 1=单线（当前交付，DATA=IO2/PB0）；2=两线（CLK=IO2/PB0、DATA=IO3/PB1） |
| `RD91_WT_TWO_BYTE_ADDR` | 0 | 0=单字节地址（00H~DFH，<224 段），1=双字节 |
| `RD91_WT_SEGMENT_ALARM` | 1 | 报警段号，**必须等于芯片内实际烧录的段号** |
| `RD91_WT_VOLUME_LEVEL` | 15 | 15=不发音量命令（芯片默认最大）；0~14 在上电后发一次 |
| `RD91_WT_BUSY_ENABLED` | 1 | 用 BUSY 确认“开始播放”；置 0 则只记录命令已发出 |
| `RD91_WT_INIT_MS` | 100 | 上电初始化等待（手册约 40ms，留余量） |
| `RD91_WT_PRE_HIGH_US` / `_START_LOW_US` | 5000 / 5000 | 每字节前数据线（单线 DATA / 两线 CLK）高 5ms、低 5ms |
| `RD91_WT_BIT_ONE_HIGH_US` / `_LOW_US` | 600 / 200 | 单线：高 600µs + 低 200µs 表示 1（3:1） |
| `RD91_WT_BIT_ZERO_HIGH_US` / `_LOW_US` | 200 / 600 | 单线：高 200µs + 低 600µs 表示 0（1:3） |
| `RD91_WT_CLK_HALF_US` | 350 | 两线：位周期 700µs（手册允许 200µs~3.2ms），单线模式不使用 |
| `RD91_WT_CMD_GAP_MS` | 2 | 命令间隔下限 |
| `RD91_WT_BUSY_TIMEOUT_MS` / `_POLL_TICKS` | 100 / 1 | 发码后等 BUSY 超时与轮询粒度 |

控制方式（一线/两线）是**烧录语音工程时固定的**，软件不能切换；到手模块按单线工程烧录，本工程默认 `RD91_WT_WIRE_MODE=1`。音频资源在 WT 芯片内部，段号不匹配时芯片会播别的内容或不播。

### 时序实现

单线位宽 200~600µs、两线位周期 700µs，都无法用 10ms tick 表达，驱动用 DWT 周期计数器做微秒延时：每个字节前 5ms 高/5ms 低的等待在**调度器锁外**，只有连续 8 位期间调用 `rt_enter_critical()`（单线约 6.4ms、两线约 5.6ms）。该函数在 RTT 3.1.4 中只增加调度器锁计数，**SysTick 与 UART 中断照常响应**，不会出现整条命令关中断。启动顺序为“播放段地址 → 等待 2ms → F2 循环 → 等 BUSY”，F2 只发一次，之后由芯片自己循环；停止发 FE。

### 期望日志

```text
[controller] WT_READY wire=1WIRE addr_mode=SINGLE seg=1 busy=1
[controller] BENCH_ALARM_SELFTEST on=1
[controller] ALARM_OUT_BUZZER on=1
[controller] ALARM_OUT_LAMP blink=1 period_ms=250
[controller] ALARM_VOICE_CMD seg=1
[controller] ALARM_VOICE_PLAYING seg=1
...
[controller] BENCH_ALARM_SELFTEST on=0
[controller] ALARM_OUT_BUZZER on=0
[controller] ALARM_OUT_LAMP blink=0
[controller] ALARM_OUT_STOP stop_us=12345 worst_us=12345
```

`ALARM_OUT_STOP` 的 `stop_us` 是“业务写入停止期望 → FE 完整发完”的实测延迟，`worst_us` 是本次上电以来的最大值。故障行：`FAULT wt_init=-1`（DWT 不可用）、`FAULT alarm_voice op=busy err=-3`（BUSY 超时，未观察到播放）、`FAULT alarm_voice op=stop/play/loop`（发码失败）、`FAULT alarm_event/alarm_thread`（IPC/线程创建失败）。任一路失败只记录该路故障，另外两路仍会执行有效的动作与停止。

### 台架操作方法

上电三路默认关闭。`RD91_BENCH_ALARM_SELFTEST=1`（默认）时：

- 按一次 PA0：启动声光自检（同时仍会翻转台架灯 PB5，并打印 `BENCH_KEY`）；再按一次停止；
- 或从调试器调用 `Bench_AlarmSelfTest(1)` / `Bench_AlarmSelfTest(0)`，适合反复做“启动→立即停止→快速交替→再启动”；
- 自检走的是与业务报警**完全相同**的 `alarm_output` sink 和同一个声光线程，不修改业务状态机（app 的 arm/alarm 状态不变）。

退出自检：把 `RD91_BENCH_ALARM_SELFTEST` 改为 0 重新编译，按键只翻转台架灯，调试器符号也不再生成。

### 逻辑分析仪核对步骤

1. 探头：PB0（单线 DATA）、PA1（BUSY），需要时加 PA2/PA3/PA4；两线模式再加 PB1。
2. 启动后抓一帧播放命令（单线）：DATA 先高约 5ms、再低约 5ms；随后 8 个位，每个位高在前、低在后，**低位先发**：600µs 高 + 200µs 低 = 1，200µs 高 + 600µs 低 = 0（含 5ms/5ms 起始，单字节约 18.4ms）。
3. 地址字节之后应看到第二条字节（F2），两字节间同样先高 5ms；两条命令间隔 ≥2ms。控制方式为两线时（`RD91_WT_WIRE_MODE=2`），CLK 在 PB0、DATA 在 PB1，位周期约 700µs，DATA 在 CLK 低电平期间变化、上升沿稳定。
4. 停止时只出现一条 FE；重复按启动不应重复发地址（`ALARM_VOICE_CMD` 不再打印）。
5. BUSY 在发码后 500µs~65ms 内变低（PWM/DAC 模式不同）；若一直为高，日志会先 `ALARM_VOICE_*` 后出现 `FAULT alarm_voice op=busy`。
6. 报警期间 console 仍应持续出现 `HEARTBEAT` 与 `IMU_MOTION_STAT`：这是“只锁调度、不关中段”的直接证据；若这两类日志停住，说明时序实现阻塞了系统时间。

## 充电/状态指示灯（台架）

红灯/绿灯分别指示“充电中/已充满”：`CHRG_STAT=01/10` → 红灯常亮；`CHRG_STAT=11` → 绿灯常亮；`PG_STAT=0`（无输入）、`CHRG_STAT=00`（有输入但充电被禁用）与读取失败（UNKNOWN）→ 全灭；充电故障 → 红灯 500ms 闪烁。判定在 [app/charge_indicator.c](app/charge_indicator.c)，适配层在 [src/charge_led.c](src/charge_led.c)，由 main 线程每 500ms 轮询一次。

**状态只从 I2C 读，不接 STAT/nPG 引脚**：硬件口径是充电器（SGM41513）与电量计（CW2217）都挂在 I2C 上，STAT/nPG 是否引出未确认（nPG 预计不接）。驱动 [drivers/sgm41513.c](drivers/sgm41513.c) 只读寄存器、不写寄存器，充电器保持上电默认自主充电模式（看门狗到期会置 WATCHDOG_FAULT 并恢复默认寄存器，本驱动不把它当充电故障）。

### 接线（开发板杜邦线，实板必须核对）

| 信号 | MCU 脚 | 方向 | 有效电平 | 说明 |
|---|---|---|---|---|
| 充电指示灯 红 | PA5 | 输出 | 见 `RD91_CHG_LED_WIRING` | 默认 3 脚共阴、高有效 |
| 充电指示灯 绿 | PA6 | 输出 | 见 `RD91_CHG_LED_WIRING` | 默认 3 脚共阴、高有效 |
| SGM41513 SCL/SDA | PB8/PB9 | 共享 I2C1 | — | 地址 0x1A；与 QMI8A01z（0x6A）共用总线，驱动内部用互斥锁串行化 |

这盏灯归 main 线程，与报警红蓝灯（PA3/PA4，声光线程）和台架灯（PB5）是不同输出，不存在两个线程驱动同一盏灯。

### 配置（[inc/project_config.h](inc/project_config.h) 第 6 组）

| 宏 | 当前值 | 含义 |
|---|---|---|
| `RD91_CHG_POLL_MS` | 500 | 采样周期；单次 I2C 事务 <1ms |
| `RD91_CHG_DEBOUNCE_MS` | 500 | 原始状态保持多久才切换（插拔/故障瞬间不闪） |
| `RD91_CHG_FAULT_BLINK_MS` | 500 | 故障时红灯亮/灭各持续多久（台架演示值） |
| `RD91_CHG_LED_WIRING` | 0 | 0=3 脚共阴高有效，1=共阳低有效，2=2 脚反并联 |
| `RD91_BENCH_CHG_SELFTEST` | 0 | 置 1 重新编译后才允许调试器调用 `Bench_ChargeSimSet()`；默认关闭以省 Flash |

### 期望日志

```text
[controller] SGM_READY addr=0x1A pn=0 rev=0 poll_ms=500
[controller] CHARGE_STATE state=CHARGING
[controller] CHARGE_STATE state=FULL
[controller] CHARGE_STATE state=NO_INPUT
[controller] CHARGE_STATE state=FAULT chrg=2 bat=0 ntc=0 reg08=0x?? reg09=0x??
```

故障行：`FAULT sgm_probe=NO_ACK addr=0x1A`（上电探测不到充电器，灯保持熄灭，不影响启动）、`FAULT sgm_read=NO_ACK addr=0x1A`（运行中读失败，只打印一次，恢复时打印 `SGM_RECOVERED`）。`CHARGE_STATE` 只在去抖后状态变化时打印。`state=FAULT` 附带解码后的故障位与原始寄存器值，便于对照手册。

### 台架操作方法

没有充电模块时，把 `RD91_BENCH_CHG_SELFTEST` 置 1 重新编译，再用调试器调用 `Bench_ChargeSimSet(state)`：

| 参数 | 模拟状态 | 期望现象 |
|---|---|---|
| -1 | 回到真实 I2C | 按实际充电器（未接则读失败，灯灭） |
| 0 | 无输入 | 全灭 |
| 1 | 充电中 | 红灯常亮 |
| 2 | 充满 | 绿灯常亮 |
| 3 | 充电故障（过热） | 红灯 500ms 闪烁 |
| 4 | 有输入但充电被禁用 | 全灭 |

模拟值走的是同一条判定与点灯路径（只是数据源换成模拟状态位），不是直接写 GPIO。调用后最多一个采样周期（500ms）+ 去抖（500ms）后反映到灯上。

未上板验证：SGM41513 实际地址/寄存器解码、REG09 “连续读两次取当前故障”的实际行为、PA5/PA6 在开发板上是否引出、双色灯接线方式、真实插拔/充满时的寄存器序列、故障闪烁是否为产品要求的提示方式。无输入/禁用/故障三种状态的颜色也未经产品确认（需求只明确“充电中红色、充满绿色”）。

## 电池电量（台架）

CW2217 电量计挂在 I2C1（地址 0x64），main 线程每 2 秒读一次 SOC，换算成 0~100% 后作为 `RD91_EV_BATTERY`（`index=RD91_BATTERY_LOCAL`）投回业务事件队列；业务状态仍由 `Rd91App` 保存，显示/上报还未接入。驱动见 [drivers/cw2217.c](drivers/cw2217.c)，换算与过期判断见 [app/battery_level.c](app/battery_level.c)，适配层见 [src/battery_gauge.c](src/battery_gauge.c)。

- 只报告电量计给出的 SOC：**不做低电阈值/告警**（产品规则 Q11 未确认），也不读电流（采样电阻与标定未确认）。
- 上电默认 shutdown，初始化必须向 CONFIG(0x08) 依次写 `0x30`、`0x00` 进入 active 模式——这是本工程对电量计的**唯一写操作**，序列来自手册 Power States。
- 读取失败不等于 0%：先保留上次有效值，连续 `RD91_BATT_STALE_MS`（6s）没有成功读取才上报“未知”（`valid=0`），显示端不得把未知当 0%。
- “是否充电”来自 SGM41513 充电器状态（充电中=1、已充满/无输入/禁用=0、未知或故障=0xFF），不是电量计的判断。

### 接线（开发板杜邦线，实板必须核对）

| 信号 | MCU 脚 | 方向 | 说明 |
|---|---|---|---|
| CW2217 SCL/SDA | PB8/PB9 | 共享 I2C1 | 地址 0x64；与 QMI8A01z（0x6A）、SGM41513（0x1A）共用总线，事务级互斥 |
| CW2217 VCELL / VDD / TS / CSP / CSN | — | 电源/电池侧 | 按原理图接电池分压、供电与 NTC；本固件不配置电池 profile、不写 TS/电流相关寄存器 |

### 配置（[inc/project_config.h](inc/project_config.h) 第 7 组）

| 宏 | 当前值 | 含义 |
|---|---|---|
| `RD91_BATT_POLL_MS` | 2000 | 采样周期（SOC 变化慢；单次约 5 条事务、约 1ms） |
| `RD91_BATT_STALE_MS` | 6000 | 连续多久没有成功读取就把百分比标为未知 |
| `RD91_BATT_LOG_MS` | 10000 | 正常读取时 `BATTERY_STAT` 的最小间隔 |
| `RD91_BENCH_BATT_SELFTEST` | 0 | 置 1 重新编译后才允许调试器调用 `Bench_BatterySimSet()`；默认关闭以省 Flash |

### 期望日志

```text
[controller] BATT_READY addr=0x64 version=0xA0 poll_ms=2000
[controller] BATTERY_STAT src=I2C percent=87 valid=1 chg=0 soc_raw=22272 mv=3981 t=25.5
[controller] BATTERY_REPORT percent=87 valid=1 chg=1
[controller] BATTERY_REPORT percent=88 valid=1 chg=1
```

故障行：`FAULT cw2217_probe=-1 addr=0x64`（读 VERSION 失败，总线/接线/供电问题）、`FAULT cw2217_probe=-3 ... version=0x??`（地址应答但版本不是 0xA0）、`FAULT cw2217_wake=...`（进入 active 失败）、`FAULT cw2217_read=NO_ACK addr=0x64`（运行中读失败，只打印一次，恢复时打印 `CW2217_RECOVERED`）、`WARN battery_soc_invalid raw=...`（SOC 原始值超出 0..100%）。`BATTERY_STAT` 每 10 秒一行；`BATTERY_REPORT` 在百分比/有效性/充电标志变化时出现，对应业务层的一条 `BATTERY` 日志。

### 台架操作方法

没有电量计时，把 `RD91_BENCH_BATT_SELFTEST` 置 1 重新编译，再用调试器调用 `Bench_BatterySimSet(percent)`：

| 参数 | 含义 |
|---|---|
| -1 | 回到真实 I2C（未接则读失败，`valid` 在 6s 后变为 0） |
| 0..100 | 模拟电量百分比 |

模拟值走同一条换算、过期判断与事件路径（日志 `src=SIM`），不是直接改业务状态。

未上板验证：CW2217 实际地址与 VERSION、CONFIG 0x30/0x00 唤醒序列的实际效果、唤醒后首次 SOC 的建立时间、SOC/VCELL/TEMP 的量程与精度、VCELL 高低字节分两次读取的实际一致性、电量计在到手板上是否上电、6s 过期时间与 2s 采样周期、`chg` 由充电器状态推导是否与产品显示要求一致。低电阈值、告警动作、跨重启持久化均未实现（Q11）。

## RTT本地适配

startup/保留原厂启动文件，将Reset_Handler调用main改为entry，使调度器先运行；linker/保留RTT初始化表并增加MSP边界断言；bsp/rt_board.c将原厂固定上半RAM heap改为静态8KiB BSS数组。没有修改SDK原件。main栈2048B（从RTT heap分配）、input栈768B、imu与声光线程各1024B静态栈、MSP2KiB，实际高水位待测。

后续BLE接入注意：RTT默认UART4为PC10/PC11，BLE需PB14/PB15。调度/中断/解析器所有权见.ai规范，不能直接拷贝裸机主循环。验证步骤见 [.ai/verification.md](.ai/verification.md)。
