# 源码架构

[返回首页](../README.md)

```mermaid
flowchart LR
    Keys[键盘按键 / H1 鼠标] --> Keyboard[键盘 ESP32-S3]
    Keyboard -->|USB HID| PC[电脑]
    Keyboard -->|BLE HID| PC
    Keyboard -->|Wi-Fi / ESP-NOW| Receiver[接收器 ESP32-S3]
    Receiver -->|USB HID| PC
    Knob[旋钮 ESP32-S3] <-->|UART LX v1| Keyboard
    Keyboard <-->|Wi-Fi / 小智协议| Cloud[小智服务]
    Config[Windows 上位机] -.->|产品 USB| Keyboard
    Config -.->|产品 USB| Receiver
```

## 键盘

入口为 `firmware/keyboard/main/main.c`。板级初始化/按键扫描在 `board.c`，USB Host 鼠标在 `usb_host_base.c`，蓝牙在 `ble_keys.c`，旋钮 UART 在 `knob_link.c`，灯光与音频偏好在 `user_controls.c`。

语音模块包括音频采集、Opus、唤醒、小智连接与官方激活、播放缓冲和语音输入。播放链路使用预缓冲及断流后的重新缓冲；音频处理的关键热数据与任务栈有不同内存要求，不要把所有内存一律挪到 PSRAM。模式保存涉及 NVS，需要符合 Flash/cache 操作限制的任务栈。

电量、休眠、低电灯效分别由 `battery_*`、`power_policy.c` 等处理。模式切换、按键映射、灯光与绑定配置保存于 NVS。

## 共用组件

| 目录 | 职责 |
| --- | --- |
| `firmware/components/bringup_usb` | 产品 USB HID / CDC、报文收发 |
| `firmware/components/input_core` | 按键状态、自动文字、媒体键等输入处理 |
| `firmware/components/hid_mouse` | HID 鼠标解析与队列 |
| `firmware/components/radio_keys` | ESP-NOW 输入及状态维护 |
| `firmware/components/network_link` | Wi-Fi、发现、认证、配置事务、控制回传 |
| `shared/protocol` | LX v2 帧编解码、CRC、流解析 |

接收器以共用组件接收无线输入并提交到 USB HID，另有独立状态灯任务。设置和绑定由各自设备保存，不使用仓库内写死的私人参数。

## 旋钮

入口为 `firmware/knob/main/app_main.c`；`components/app` 协调子系统，`event` 提供事件总线。`haptics` 管理 SimpleFOC 和触觉，`input_strain` 管理 HX711 与校准，`ui` 使用 LVGL 8.3.11，`host_link` 与键盘通信，`ble_dial` 提供独立 Dial 功能。

校准兼容版本由 `project_build_id` 固定，不随编译时间变化。界面图片已经转换为可编译的 C 数组，正常构建不依赖原始设计工程。

## 依赖原则

组件路径均指向工程内 `third_party`，版本与内容由 `third_party.lock.json` 固定。库升级、分区变更、硬件换型和协议变更应分开验证；不得为了通过本地构建改回某个开发者的绝对路径。
