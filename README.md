# rd91-controller-fw — 中控

N32WB436RFQ7 / 厂家RT-Thread3.1.4 / Arm GCC。当前为P0台架骨架，后续由Claude Code继续开发。先审核 [.ai/agent-prompt.md](.ai/agent-prompt.md)；设计与来源见 [.ai/README.md](.ai/README.md)。

## 当前内容

RTT启动、main/input线程、静态消息队列、USART3日志、PB5灯、PA0消抖按键。app/为不依赖RTT的普通C；线程与时间、日志适配位于src与bsp。

传感器：QMI8A01z 已接入 I2C1，驱动位于 [drivers/](drivers/)、总线访问位于 [bsp/i2c1.c](bsp/i2c1.c)。IMU 采样线程以 100Hz 连续读取新样本，同时驱动两条互不影响的链路：

- 运动状态（停车/行驶）：[app/motion_stats.c](app/motion_stats.c)，对每 1 秒窗口计算去均值三轴 RMS，两个阈值加连续窗口确认，输出 UNKNOWN/STILL/MOVING；
- 疑似冲击：[app/shock_detect.c](app/shock_detect.c)，逐样本比较三轴合矢量 |a|，聚合成事件并记录峰值、时长、饱和标志，只输出“疑似冲击”，不进入报警/喷烟。

芯片的 Any-Motion 中断仍然保留，只作为“发生扰动”的唤醒信号，与冲击峰值、车辆运动判断是三种不同的事实。当前没有 STOP2 深睡，也没有 BLE、执行输出或布撤防业务。出厂默认撤防已写入需求。

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

## RTT本地适配

startup/保留原厂启动文件，将Reset_Handler调用main改为entry，使调度器先运行；linker/保留RTT初始化表并增加MSP边界断言；bsp/rt_board.c将原厂固定上半RAM heap改为静态8KiB BSS数组。没有修改SDK原件。main栈2048B、input栈768B、MSP2KiB，实际高水位待测。

后续BLE接入注意：RTT默认UART4为PC10/PC11，BLE需PB14/PB15。调度/中断/解析器所有权见.ai规范，不能直接拷贝裸机主循环。验证步骤见 [.ai/verification.md](.ai/verification.md)。
