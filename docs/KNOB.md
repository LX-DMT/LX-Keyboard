# 旋钮使用说明

[返回首页](../README.md) · [工程入口](../knob/README.md)

## 基本操作

- 旋转：切换菜单项或调节当前值。
- 短按：确认。
- 长按约 1.4 秒：返回主菜单；独立 Dial 模式长按约 3 秒退出。

菜单名称以当前固件英文 UI 为准。旋钮通过 UART 与键盘通信，既发送操作也接收电量、音频状态和电脑负载；仅通电而 UART 未接通时不能控制键盘。

| 菜单 | 用途 |
| --- | --- |
| Remote Link | 选择键盘四种连接模式 |
| LED | 灯效和颜色 |
| Audio | 控制音频开关；顺时针开启、逆时针关闭，接收键盘状态回传 |
| Standby → Power | 查看键盘电压、电流等电量信息 |
| Standby → Instrument | 显示电脑 CPU / GPU / 内存数据，需要外部软件提供数据 |
| Dial | 旋钮独立以 BLE Surface Dial 工作，配对名 `LX_Dial` |
| Strain Cal | 重新进行压力/电机校准 |

`LX_Dial` 是旋钮设备，`LX Keyboard Dev` 是键盘设备，两者不是同一个蓝牙输入设备。

## 首次校准

空白设备没有有效校准数据时进入校准界面。先固定好机械结构，按照屏幕提示操作：

1. `Rotate the knob to start calibration`：旋转约 20° 启动。
2. `Starting calibration`：等待。
3. `Keep pressing`：保持按压约 3 秒。
4. `Keep released`：松开约 3 秒。
5. `Motor aligning`：等待电机对中，不要强行阻挡旋转。
6. `Calibration complete!`：完成后进入菜单。

校准包括压力阈值和 FOC 参数，写入 NVS。普通重编译、换目录或 SDK 重新安装不会因时间戳变化而使校准失效；当前兼容版本固定为 0。更换电机、压力传感器或改变机械结构后需要重新校准，不要复制另一台旋钮的校准数据。

## 与键盘连接

键盘 TX14 → 旋钮 RX44；旋钮 TX43 → 键盘 RX21，并共地。核对[板级电源和引脚说明](HARDWARE.md)，UART 逻辑为 3.3 V，不能接 RS-232 电平。旋钮 UART0 同时用于日志、下载与键盘协议，下载器或调试程序不应与键盘同时驱动同一 RX 线。

键盘休眠可断开旋钮供电。编译烧录旋钮时应保持供电；使用一键脚本的 `--power-port` 可让键盘保持唤醒，具体见[编译烧录](BUILD_FLASH.md)。

## 调试仪表显示

`knob/tools/host_link_tool.py` 是开发用 UART 工具，不是完整上位机。进入 Instrument 页并确保 UART 无其他发送方后，可使用：

```sh
python knob/tools/host_link_tool.py stats DOWNLOAD_PORT --cpu 25 --gpu 17 --ram 51
```

这会发送固定演示数值，不代表电脑实际负载。工具依赖 IDF Python 环境中的 pyserial。`monitor` / `stats` / `demo` 使用同一串口，不能同时运行；不指定串口会直接报错。
