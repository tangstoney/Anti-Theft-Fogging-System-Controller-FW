# 接口与N32例程映射

出厂撤防已确认。旧图TI引脚编号忽略，正式板IO以硬件最新图纸为准。当前以N32WB43X_STB V1.0资料作为开发板参考，供应商到手板的PCB版本/LED极性/按键须核对。

| 资源 | 当前工程或后续参考 | 说明 |
|---|---|---|
| RTT console | USART3 PB10 TX(AF0)、PB11 RX(AF5)，115200 8N1 | 沿用RT_Thread10；P0仅日志，未装CLI/Finsh接收线程。通常需3.3V USB-TTL共地 |
| 参考板J3 DEBUG串口 | 通常接USART1 PA9/PA10 | 不能指望当前USART3日志直接出现在J3；需要时按厂商驱动切console至usart1并核对跳线 |
| 台架LED | PB5，高有效暂定 | SDK GPIO/LedBlink起点，实板核对后集中修改bsp/board.h |
| 台架按键 | PA0对GND，内部上拉，30ms消抖 | P0线程轮询，无EXTI占用；按住不连发，上电按住先等待释放 |
| RTT tick | 100Hz，即10ms/tick | 沿用RTT例程；now_ms适配集中实现，不另开SysTick |
| 系统时钟 | SDK system_n32wb43x.c默认HSE PLL 108MHz | 晶振按开发板核对，不沿用旧TI图48MHz假设 |
| 后续M4↔M0 | UART4 PB14 TX/PB15 RX，AF6，115200 | BLE/master/inc/app_uart.h；RTT驱动默认却是PC10/PC11，集成时必须本地适配 |
| 后续M0控制 | PC6 RESET、PC9 WAKE、PC8 STATUS、PC7 IRQ | BLE/master/src/app_io.c；整组保留，P0未启用 |
| SWD | PA13/PA14 | 按实板核对，保持调试可用 |
| IMU QMI8A01z | PB8/PB9 I2C1 AF4，配置 400kHz（`RD91_I2C1_CLK_HZ`，100kHz 也够用）；PC0 接 IMU INT1，EXTI0 上升沿 | 采样线程 100Hz（每 10ms 一帧，ODR 1000Hz、±8g、芯片 LPF 关闭）。两条独立链路：秒级振动 RMS 推断停车/行驶；逐样本 \|a\| 判疑似冲击。Any-Motion 仅作扰动唤醒（CTRL8.bit6=1 指向 INT1）。从机地址 0x6A(SA0 高/悬空)/0x6B 待实板确认；双 IMU 的地址/IRQ 后续分别确认 |
| 后续人体雷达 | PC1 OUT/EXTI1，UART调参另接 | 按实际觅感厂家手册；不能依旧图SPI。供电5~12V，先核对电平再接GPIO |
| WT588F02 DATA（IO2，单线） | PB0 输出，空闲低；字节前高5ms→低5ms；1=高600µs+低200µs，0=高200µs+低600µs，LSB先发 | **当前按单线交付**（到手模块烧录为单线工程）：单线数据输入就是 IO2，与两线 CLK 同脚。串1k建议；**IO2 严禁外部上拉**，上电为高可能进入非工作模式 |
| WT588F02 IO3（两线 DATA） | PB1 输出，空闲低，上升沿采样 | 单线模式不接、驱动为低；仅当 `RD91_WT_WIRE_MODE=2` 时作两线 DATA，与 PB0(CLK) 配合 |
| WT588F02 DATA（IO3） | PB1 输出，空闲低，上升沿采样，LSB先发 | 位周期700µs（350/350），命令间隔≥2ms；FLASH 内音频，段号须与烧录一致 |
| WT588F02 BUSY（IO1） | PA1 输入，内部上拉；播放低、空闲高 | 未接模块时读为高，BUSY 等待超时记故障，不当作播放成功。WT 若 5V 供电，3.3V 控制与回读均不达 VIH=0.8VCC |
| 本机蜂鸣器 | PA2 输出，有效电平见 `RD91_BUZZER_ACTIVE_HIGH` | 有源蜂鸣器 GPD4216L100-3-36V2800（3~36V、≤35mA、≥100dB@10cm）经外部三极管/MOS 驱动，GPIO 不直接带负载 |
| 本机红蓝灯 | 红 PA3、蓝 PA4，接线方式见 `RD91_LAMP_WIRING` | 默认 3 脚共阴高有效；250ms 交替为台架演示值。与台架灯 PB5 分属不同线程，不共用 |
| 充电指示灯 | 红 PA5、绿 PA6，接线方式见 `RD91_CHG_LED_WIRING` | 充电中红、充满绿；由 main 线程独占，与报警灯/台架灯不是同一输出 |
| SGM41513 充电器 | I2C1（PB8/PB9）从机地址 0x1A；REG08/REG09 | 硬件口径：充电器与 CW2217 电量计都在 I2C 上；STAT/nPG 是否引出未确认（nPG 预计不接），状态只读寄存器。只读不写，保持默认自主充电模式 |
| CW2217 电量计 | I2C1（PB8/PB9）从机地址 0x64；VERSION=0xA0 | 上电默认 shutdown，初始化写 CONFIG(0x08)=0x30 再 0x00 进入 active（唯一写操作）；读 SOC(0x04/0x05)、VCELL(0x02/0x03)、TEMP(0x06)。VCELL/VDD/TS/CSP/CSN 的电池侧接线按原理图核对 |

开发板大部分接口可沿用原厂例程，新增杜邦线接线在README记录信号、MCU脚、排针、有效电平、电压和来源。优先少改原厂AF/时钟/总线配置；若存在冲突，明确局部适配，不擅自重新分配整板IO。

当前bsp/rt_board.c基于UART-GCC板级初始化，heap改为显式BSS数组；startup本地副本仅将bl main改为bl entry；linker本地副本增加堆栈边界检查。原始SDK保持不变。UART3驱动沿用SDK，未启用UART4或M0。

32KiB RAM中8KiB为RTT heap静态预留，MSP预留2KiB；main栈2048B从RTT heap分配，input栈768B静态，imu采样栈1024B静态，声光线程栈1024B静态（另有 `rt_event` 与快照静态存放）。消息队列16槽，每槽保存一个 `Rd91Event`（约24B）并额外保留一个内部链表指针；上板后量测四个线程的栈高水位，后续加BLE需重新预算。P0无STOP2、无整机功耗策略。

声光输出所有权：语音/蜂鸣器/红蓝灯只由声光线程驱动，业务层只写期望状态与版本号；台架灯 PB5 仍归 main 线程。语音位时序用 DWT 微秒延时，仅连续 8 位（约5.6ms）期间锁调度器，SysTick/UART 中断保持开启；上板须用逻辑分析仪核对时序并确认报警期间 tick/IMU 日志不中断。

充电指示灯同样归 main 线程：PA5/PA6 只由 `Board_ChargeLedDrive` 驱动，状态来自 SGM41513 的 I2C 寄存器，不占 GPIO 输入。I2C1 由 IMU 采样线程（优先级 8）与 main 线程（优先级 10）共用，bsp/i2c1.c 用 RTT 互斥锁串行化整条事务（含重复 START 与收尾），单次事务 <1ms；禁止在中断里调用 I2C 接口。充电器只读不写，I2C 探测/读失败都只记故障，不影响启动与其他功能。

电量计（CW2217，0x64）也由 main 线程每 2s 轮询一次，同样走 I2C1 互斥锁；它与充电器、IMU 三个器件共用总线，任何新增 I2C 器件都必须走同一接口，不要另建绕过锁的事务。电量计初始化会写 CONFIG 两次（0x30、0x00）以退出 shutdown，这是全工程唯一对电量计的写操作；其余状态读取失败只报未知，不伪造 0%。
