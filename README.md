# rd91-controller-fw — 中控

N32WB436RFQ7 / 厂家RT-Thread3.1.4 / Arm GCC。当前为P0台架骨架，后续由Claude Code继续开发。先审核 [.ai/agent-prompt.md](.ai/agent-prompt.md)；设计与来源见 [.ai/README.md](.ai/README.md)。

## 当前内容

RTT启动、main/input/imu/声光四个线程、静态消息队列、USART3日志、PB5灯、PA0消抖按键。app/为不依赖RTT的普通C；线程与时间、日志适配位于src与bsp。

传感器：QMI8A01z 已接入 I2C1，驱动位于 [drivers/](drivers/)、总线访问位于 [bsp/i2c1.c](bsp/i2c1.c)。IMU 采样线程以 100Hz 连续读取新样本，同时驱动两条互不影响的链路：

- 运动状态（停车/行驶）：[app/motion_stats.c](app/motion_stats.c)，对每 1 秒窗口计算去均值三轴 RMS，两个阈值加连续窗口确认，输出 UNKNOWN/STILL/MOVING；
- 疑似冲击：[app/shock_detect.c](app/shock_detect.c)，逐样本比较三轴合矢量 |a|，聚合成事件并记录峰值、时长、饱和标志，只输出“疑似冲击”，不进入报警/喷烟。

芯片的 Any-Motion 中断仍然保留，只作为“发生扰动”的唤醒信号，与冲击峰值、车辆运动判断是三种不同的事实。当前没有 STOP2 深睡，也没有 BLE、喷烟执行输出或布撤防业务；本机声光报警已实现（含台架自检入口，见下文）。出厂默认撤防已写入需求。

## 构建

```powershell
cd "D:\Tangshi\Project\N32Demo\Anti-Theft Fogging System\rd91-controller-fw"
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
# 全量重编译：
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean
```

默认工具链为用户目录.n32studio/Toolchain；SDK为../../_sdk/Nsingtech.N32WB43x_Library.2.2.0，保持只读。可用-SdkRoot/-GccPath/-MakePath覆盖；SDK与工具链路径采用无空格路径，本仓库父目录可带空格。VS Code提供构建任务。

Linux CI可使用同版本SDK/Arm GNU执行make SDK_ROOT=/opt/n32-sdk；本次未运行Linux或配置Runner。

产物build/rd91_controller.elf、.hex、.bin、.map。M4向量表0x08000000，不含M0镜像/Bootloader/OTA。实际构建结果见build-summary.json和logs/build.log，硬件未验证。

## 台架接线与输出

日志USART3：PB10 TX(AF0)、PB11 RX(AF5)，115200 8N1；当前只输出日志。USB-TTL RX接PB10并共地。参考板J3常接USART1，当前需外接USB-TTL或按board.md改console。

LED PB5（高有效暂定）、按键PA0对GND/内部上拉。RTT tick=100Hz，输入线程每10ms采样，30ms消抖；每次稳定按下只打印一次BENCH_KEY并翻转台架灯。BOOT注明BENCH_STUB_IO、BLE未实现、sensors=IMU_ANYMOTION sample=100Hz，每秒HEARTBEAT。

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
| WT588F02 CLK（IO2） | PB0 | 输出 | 空闲低；起始高 5ms → 低 5ms | 建议串 1k 电阻。**IO2 严禁外部上拉**：上电为高可能让芯片进入非工作模式 |
| WT588F02 DATA（IO3） | PB1 | 输出 | 空闲低，上升沿采样，LSB 先发 | 串 1k；与 CLK 一起先置低再切输出 |
| WT588F02 BUSY（IO1） | PA1 | 输入 | 播放时低，空闲高 | MCU 内部上拉；模块未接时读为高 → BUSY 超时报故障，不会误报“正在播放” |
| 蜂鸣器驱动 | PA2 | 输出 | 高 = 导通（`RD91_BUZZER_ACTIVE_HIGH`） | 经外部三极管/MOS 驱动级；有源蜂鸣器 3~36V、≤35mA，GPIO 不直接带负载 |
| 红灯 | PA3 | 输出 | 见 `RD91_LAMP_WIRING` | 默认 3 脚共阴、高有效 |
| 蓝灯 | PA4 | 输出 | 见 `RD91_LAMP_WIRING` | 默认 3 脚共阴、高有效 |

选脚避开了 PB5（台架灯）、PA0（按键）、PB8/PB9（I2C1）、PC0（IMU INT）、PB10/PB11（console）、PA13/PA14（SWD）、PB14/PB15（未来 UART4）和 PC6~PC9（未来 M0 控制）。台架灯 PB5 仍由 main 线程按键翻转，与报警灯不是同一盏灯，不存在两个线程驱动同一输出。供电：WT 若按 5V 供电，VIH=0.8VCC=4.0V，3.3V 控制信号不达高电平门限——**要么 WT 也用 3.3V 供电，要么加电平转换**；BUSY 回读电平同理。

### 语音芯片配置（[inc/project_config.h](inc/project_config.h)）

| 宏 | 当前值 | 含义 |
|---|---|---|
| `RD91_WT_WIRE_MODE` | 2 | 两线串口；一线模式未实现，改成 1 会编译报错 |
| `RD91_WT_TWO_BYTE_ADDR` | 0 | 0=单字节地址（00H~DFH，<224 段），1=双字节 |
| `RD91_WT_SEGMENT_ALARM` | 1 | 报警段号，**必须等于芯片内实际烧录的段号** |
| `RD91_WT_VOLUME_LEVEL` | 15 | 15=不发音量命令（芯片默认最大）；0~14 在上电后发一次 |
| `RD91_WT_BUSY_ENABLED` | 1 | 用 BUSY 确认“开始播放”；置 0 则只记录命令已发出 |
| `RD91_WT_INIT_MS` | 100 | 上电初始化等待（手册约 40ms，留余量） |
| `RD91_WT_CLK_PRE_HIGH_US` / `_START_LOW_US` | 5000 / 5000 | 每字节前 CLK 高 5ms、低 5ms |
| `RD91_WT_CLK_HALF_US` | 350 | 位周期 700µs（手册允许 200µs~3.2ms） |
| `RD91_WT_CMD_GAP_MS` | 2 | 命令间隔下限 |
| `RD91_WT_BUSY_TIMEOUT_MS` / `_POLL_TICKS` | 100 / 1 | 发码后等 BUSY 超时与轮询粒度 |

控制方式（一线/两线）是**烧录语音工程时固定的**，软件不能切换；上板前必须确认到手模块烧录的是两线工程。音频资源在 WT 芯片内部，段号不匹配时芯片会播别的内容或不播。

### 时序实现

两线位周期 700µs 无法用 10ms tick 表达，驱动用 DWT 周期计数器做微秒延时：每个字节前 5ms 高/5ms 低的等待在**调度器锁外**，只有连续 8 位（约 5.6ms）期间调用 `rt_enter_critical()`。该函数在 RTT 3.1.4 中只增加调度器锁计数，**SysTick 与 UART 中断照常响应**，不会出现整条命令关中断。启动顺序为“播放段地址 → 等待 2ms → F2 循环 → 等 BUSY”，F2 只发一次，之后由芯片自己循环；停止发 FE。

### 期望日志

```text
[controller] WT_READY wire=2WIRE addr_mode=SINGLE seg=1 busy=1
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

1. 探头：PB0（CLK）、PB1（DATA）、PA1（BUSY），需要时加 PA2/PA3/PA4。
2. 启动后抓一帧播放命令：CLK 先高约 5ms、再低约 5ms；随后 8 个位周期各约 700µs（高 350µs、低 350µs），DATA 在 CLK 低电平期间变化、上升沿稳定，**低位先发**。
3. 地址字节之后应看到第二条字节（F2），两字节间 CLK 同样先高 5ms；两条命令间隔 ≥2ms。
4. 停止时只出现一条 FE；重复按启动不应重复发地址（`ALARM_VOICE_CMD` 不再打印）。
5. BUSY 在发码后 500µs~65ms 内变低（PWM/DAC 模式不同）；若一直为高，日志会先 `ALARM_VOICE_*` 后出现 `FAULT alarm_voice op=busy`。
6. 报警期间 console 仍应持续出现 `HEARTBEAT` 与 `IMU_MOTION_STAT`：这是“只锁调度、不关中段”的直接证据；若这两类日志停住，说明时序实现阻塞了系统时间。

## RTT本地适配

startup/保留原厂启动文件，将Reset_Handler调用main改为entry，使调度器先运行；linker/保留RTT初始化表并增加MSP边界断言；bsp/rt_board.c将原厂固定上半RAM heap改为静态8KiB BSS数组。没有修改SDK原件。main栈2048B（从RTT heap分配）、input栈768B、imu与声光线程各1024B静态栈、MSP2KiB，实际高水位待测。

后续BLE接入注意：RTT默认UART4为PC10/PC11，BLE需PB14/PB15。调度/中断/解析器所有权见.ai规范，不能直接拷贝裸机主循环。验证步骤见 [.ai/verification.md](.ai/verification.md)。
