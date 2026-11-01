# 协议与配置边界

[返回工程](../README.md)

## 两套协议

| 协议 | 链路 | 实现 |
| --- | --- | --- |
| LX v1 | 键盘 ↔ 旋钮 UART，115200 8N1 | [旋钮协议文档](https://github.com/LX-DMT/LX-Keyboard/blob/main/knob/docs/host_link/proto.md) |
| LX v2 | 产品 CDC、键盘/接收器无线、配置和控制 | [头文件](../components/protocol/include/lx_protocol.h)、[C/Python 实现](../components/protocol/) |

LX v2 最大帧 250 字节，字段为 `4C 58` 帧头、版本 2、类型、uint16 载荷长度、uint32 序列号、uint32 启动会话、载荷、CRC16。多字节整数小端；载荷最大 234 字节，CRC16-CCITT 初值 `FFFF`、多项式 `1021`，覆盖 CRC 前全部字节。

| 类型 | 编号 |
| --- | --- |
| KEYBOARD / ACK / KEEPALIVE | 1 / 2 / 3 |
| LED_STATE（预留） / MOUSE / CONSUMER | 4 / 5 / 6 |
| HELLO / RELEASE / HOST_STATS | 7 / 8 / 9 |
| TEXT / CONFIG（含保留字段） | 10 / 11 |
| CONTROL / CONTROL_REPLY | 12 / 13 |
| SETUP / SETUP_REPLY | 14 / 15 |
| ACTIVATION / ACTIVATION_REPLY | 16 / 17 |
| CUSTOM_KEYS / CUSTOM_KEYS_REPLY | 18 / 19 |
| LIGHTING / LIGHTING_REPLY | 20 / 21 |

协议枚举中的保留编号不代表启用相应功能；本版不提供 Wi-Fi 直连电脑输入和上位机代打。接入时以当前处理函数的参数校验与返回值为准。

## 首次配置事务

[device_setup.c](../components/network_link/device_setup.c) 实现产品 USB 配置事务：读取身份 → 开始会话 → 暂存网络和对端身份/密钥 → 验证 → 提交 → 封存。中途失败可回滚，未封存事务在重启时恢复原配置。每台设备保存的是对端身份和共享密钥，双方均需配置。

`SETUP` op1 查询；op2 用 16 字节非零随机会话 token 开始；其余会话请求携带同一 token。op3 暂存 Wi-Fi；op4 暂存对端 6 字节 MAC 与 16 字节配对密钥；op6 验证；op7 状态；op8 提交；op9 回滚；op10 封存；op11/12 扫描/读取 AP。op5 PC 配对在本版拒绝。

这是一份阅读源码的索引，不是可直接执行的配置脚本。客户端需处理双设备失败回滚、会话超时和确认，不能只发送提交就视为成功。状态读取不回显网络密码和绑定密钥。发布副本未配置时返回无对端，不回退到开发板地址。

## 数据链路说明

接收器模式由键盘选择。Wi-Fi 输入使用 TCP；ESP-NOW 保持报文边界，USB/TCP 按长度及 CRC 解析。状态心跳用于修复释放状态，断线后不会重放所有离线短按。接收器 ACK 表示接收端处理，并不等于目标应用已收到文字。

电脑负载信息可通过控制回传到键盘，再编码为旋钮 LX v1 帧；USB、BLE 与接收器控制路径的可用性不同于实体输入模式。USB 配网、自定义按键、灯光配置目前不能据此推断为全通道配置。

小智服务使用自己的 WebSocket / 音频协议，与 LX 帧协议独立。
