# Claude Agent 设计与开发规范

日期：2026-11-11。三个模块以 N32WB436 开发板进行台架验证。先读本文件、requirements.md、board.md、module.md、protocol.md、tasks.md、verification.md；具体执行入口是 agent-prompt.md。

## 已确认输入

- 出厂默认撤防，已由唐石明确确认，不再把默认布防列为待确认。
- 旧框图TI引脚编号忽略；接线优先沿用N32官方例程和到手开发板。
- 雷达以实际厂家资料为准，不能沿用旧图SPI标注。
- 操作系统先沿用厂家SDK自带RT-Thread，当前源码版本3.1.4；不自动升级、不换FreeRTOS。
- 可以建立启动工程；提示词先供唐石审核，后续由Claude Code开发验证。用户已申请更多板卡，可用杜邦线接模块。

## 工程架构

- app/：普通C业务与状态机，不依赖n32wb43x.h、rtthread.h、设备句柄或printf。时间now_ms、输入事件、输出回调由外部传入。当前仅台架启动和按键计数，不包含产品报警业务。
- bsp/：开发板初始化、RTT系统时钟/console/heap、台架GPIO。
- src/：RTT线程与消息队列、输入采集、业务调用和日志适配。
- inc/rtconfig.h：本仓库RTT配置；linker/和startup/：保留厂商版权的必要本地适配。
- 后续drivers/按模块分驱动、services/管理BLE AT和协议；按实际任务创建，空实现不得返回成功。

保持SDK只读。默认SDK_ROOT=../../_sdk/Nsingtech.N32WB43x_Library.2.2.0，可覆盖；不要依赖兄弟仓库源码。三个仓库可独立构建；同一协议版本需同步三端。量产板建立独立引脚配置，不把台架接线写死在业务里。

## RTT设计约束

使用厂家RTT3.1.4和GCC Cortex-M4上下文文件context_gcc.S。先用已有UART-GCC及SDK RT_Thread10/4/8例程；现有BLE master/slave是裸机AT示例，集成RTT需要适配，不能直接拼两个main/中断文件。

P0只有main业务线程（RTT创建）、一个静态input线程和一个静态imu采样线程：input通过静态消息队列投递消抖按键，imu按100Hz读取QMI8A01z并跑两条检测链路，事件同样经队列投递；状态只由业务线程修改。后续BLE使用独立线程独占AT事务与解析，传感器由采集线程汇总事件。先最少线程，测量后再拆分；线程优先级、栈、队列容量和最坏处理时间都写入文档。

ISR遵循rt_interrupt_enter/leave，只清标志/搬数据/非阻塞通知；不能等待互斥锁、阻塞、长日志或做I2C业务。使用有界队列，检查rt_mq_send/recv与线程初始化结果，明确溢出计数/恢复；volatile不等于同步。业务输出不能在定时器/中断回调直接执行。

RT_TICK_PER_SECOND当前100，一tick=10ms；禁止把rt_thread_delay(100)理解成100ms。tick→毫秒集中适配，使用无符号差值处理回绕。消抖30ms/心跳1s均为台架演示参数。队列接收有有限超时以处理业务定时；线程无工作时阻塞/让出CPU，不忙等。未确认前不拷贝原厂STOP2直接关时钟，RTT tick、时基和外设恢复要统一设计。

当前M4仅32KiB RAM。线程/队列尽量静态分配，RTT heap使用显式8KiB BSS数组，避免原厂固定上半RAM heap与全局变量/MSP重叠。链接脚本保留MSP2KiB并做ASSERT；实际栈余量必须上板量测，编译占用不包含动态heap已用量。

启动文件Reset_Handler须经过SystemInit、C运行时初始化，再调用RTT entry；RTT建立main线程后才调用应用main。仅给链接器加-e entry不能改变烧录后向量表复位路径。SysTick只由RTT维护；PendSV/HardFault由RTT上下文实现；不得再加入裸机同名handler。

## BLE与模块接口

N32 M4通过UART4与M0 AT固件通信。RTT厂商UART4默认PC10/PC11，BLE示例用PB14/PB15 AF6，必须改本仓库BSP映射，不能直接启用RT_USING_UART4照抄；DMA/USART IRQ只允许一套驱动拥有。console默认沿用RTT USART3 PB10/PB11，与BLE UART4独立。

先两板跑通，再验证多连接能力；当前例程不能证明中控同时连接钥匙、烟罐和手机。AT cfg_ble无界等待必须变成有超时的线程状态机；新接收数据与业务回执分开。参考已有BLE-master的GCC汇编/日志适配，但RTT console不重复注册newlib _write。

保留独立M0固件版本/INFO记录。M4独立应用起点0x08000000，不含Bootloader/OTA/M0镜像。引入BLE app_flash.c前审计0x0801FC00参数页，先在本仓库链接布局保留，再实现配置/绑定存储。

## 开发和验收

先读来源再写代码，已确认规则直接实现；缺失业务规则记入待确认表，可以继续无依赖任务。初期执行端用LED/日志替身，真实执行接口按硬件与确认条件另接入。区分传输成功、业务接受、模拟执行与物理反馈，不能用AT OK当执行成功。

台架无配对/固定MAC不是安全方案。RSSI不能单独鉴权或保证防中继；CRC、序号也不是密码学防重放。密钥不得写源码/日志。量产安全机制单独设计，不自创加密。

Windows使用Arm GNU+Make，Linux CI复用同版本工具链/SDK；无需Keil许可证。路径含空格的本工程要实际构建验证。逐次交付改动说明、实际命令/结果、接线与待验证项。编译、烧录、业务、功耗分别记录。优先本地资料，补充连接首次超时就停止重试。不自动提交、推送或修改远端。
