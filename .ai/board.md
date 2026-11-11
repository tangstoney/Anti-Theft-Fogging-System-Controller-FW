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

开发板大部分接口可沿用原厂例程，新增杜邦线接线在README记录信号、MCU脚、排针、有效电平、电压和来源。优先少改原厂AF/时钟/总线配置；若存在冲突，明确局部适配，不擅自重新分配整板IO。

当前bsp/rt_board.c基于UART-GCC板级初始化，heap改为显式BSS数组；startup本地副本仅将bl main改为bl entry；linker本地副本增加堆栈边界检查。原始SDK保持不变。UART3驱动沿用SDK，未启用UART4或M0。

32KiB RAM中8KiB为RTT heap静态预留，MSP预留2KiB；main栈2048B从RTT heap分配，input栈768B静态，imu采样栈1024B静态。消息队列16槽，每槽保存一个 `Rd91Event`（约24B）并额外保留一个内部链表指针；上板后量测三个线程的栈高水位，后续加BLE需重新预算。P0无STOP2、无整机功耗策略。
