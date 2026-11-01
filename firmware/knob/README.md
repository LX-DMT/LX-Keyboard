# 旋钮固件

ESP32-S3 / GC9A01 240×240 圆屏 / SimpleFOC / LVGL 8.3.11，ESP-IDF **5.5.6**。

- [使用与首次校准](../../docs/KNOB.md)
- [编译烧录](../../docs/BUILD_FLASH.md)
- [板级接线](../../docs/HARDWARE.md)
- [UART 协议](docs/host_link/proto.md)
- [架构](../../docs/ARCHITECTURE.md)

从仓库根目录运行 `python scripts/build.py knob`。第三方组件已固定在仓库内，无需其他工程目录。

## 子系统

`components/app` 协调启动；`event` 事件总线；`haptics` 电机与触觉；`input_strain` 压力校准；`ui` 页面；`display` 屏幕；`host_link` 键盘通信；`ble_dial` 独立蓝牙旋钮；`project_build_id` 固定校准兼容版本。

正常构建直接使用 `components/ui/assets` 的 C 资源。`tools/regen_assets_lv8.py` 是资源转换工具，所需 `src/img` 不在本包中，不属于正常编译步骤。需要重新设计素材时使用自己的源图，确认授权后转换。

当前校准兼容版本为 0，普通重编译不使已有校准失效。全新设备仍需要完成压力和电机校准。
