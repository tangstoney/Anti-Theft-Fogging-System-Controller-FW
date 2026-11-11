# 跨模块通信设计草案

状态：未批准的设计约束；尚未分配正式命令码、UUID 或二进制格式；本次无无线实现。三仓库此文件初始内容相同，任何协议修改必须同步三端并记录版本兼容性。

## 分层与命令

业务命令与 BLE AT 传输分离。先做 Key→Controller ARM/DISARM、Controller→Key 业务回执，再分别做 Controller→Fogger SIMULATE_EXECUTE 和结果回执。台架本地按键自检不等于这些链路已实现。

消息设计至少考虑 version、message_type、source/target、session、request_id、payload_length、payload。编码时明确字节序、长度上限及重组边界，禁止直接发送 C struct 内存；解析不依赖输入 NUL 终止。首轮尽量放进 MTU23 的可用业务载荷，超过则先定义分片，不能只按数组容量发送。

序号用于匹配回执和去重，不是密码学防重放。身份认证需要独立的已确认机制。重复请求应返回先前结果，不重复执行；跨重启的幂等与持久化需要专项设计。MAC、名称、UUID 不作身份凭据。

## 回执与异常

区分 TRANSPORT_SENT、BUSINESS_ACCEPTED、EXECUTION_STARTED、EXECUTION_RESULT、UNKNOWN。原厂 AT OK/sendack 只能表示对应传输阶段，不能算喷烟成功。业务回执必须匹配会话、请求、来源和类型；旧包、错端包、迟到包不得改变新请求状态。

台架 ACK 超时可配置为1000ms，只是演示值，后续需实测确认。产品要求失败重试与上报（R09），具体重试规则尚缺；首轮原型超时报告 UNKNOWN/TIMEOUT，不自动重发执行。落实执行端幂等、重试次数、间隔和有效期后才能开启产品策略。

ARM/DISARM 是设定目标状态，禁止用“翻转状态”作为跨无线协议。遥控器收到匹配业务回执才指示成功；本地发送成功或断链不能算成功。与车主授权、车辆状态判定解耦。

## N32 AT 例程事实

SDK master/inc/app_user_config.h 和 slave 对应文件的示例服务 C0FE、特征 C1FE/C2FE，须实测其读写/通知方向与 CCCD。AT手册默认服务为 FFC0/FFC1/FFC2，与示例不是同一组，不能混用。

原厂 master 用固定 MAC 自动连接且扫描关闭；slave 收到即回环。移植时停掉 master 定时256字节测试流，替换 slave 自动回环；不要直接把原厂测试 MAC 复制给每块实板。先查询实际 MAC/INFO，并核对 MAC 字节顺序（A1 §1.3.4）。

M4通过 UART4 与M0通讯，需确认模块镜像兼容性。AT固件并发能力未证实，不能仅凭 M4 有多个串口就宣称 BLE 多连接。完成两板验收后，根据供应商确认与实际连接能力决定三节点方案；更换BLE拓扑时保留业务层接口。
