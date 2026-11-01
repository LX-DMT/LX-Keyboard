# 蓝星旋钮固件

本目录为独立 ESP32-S3 工程，使用 **ESP-IDF 5.5.6**。

## VS Code 编译与烧录

1. 安装乐鑫 ESP-IDF 扩展，选择已安装的 **5.5.6** 环境。
2. 在 VS Code 中直接打开本目录（`knob`）。
3. 点击扩展的 **Build Project / 编译项目**，首次构建自动联网下载固定版本依赖。
4. 接好对应设备的下载接口，选择串口，点击 **Flash Device / 烧录**。

无需打开上级仓库，不需要运行预处理脚本。`sdkconfig.defaults` 已指定 ESP32-S3 和硬件参数。首次下载需要联网；`build/` 和 `managed_components/` 自动生成，不随源码分发。请保留 `dependencies.lock`。

命令行等价操作（ESP-IDF 5.5.6 环境终端）：

```sh
idf.py build
idf.py -p DOWNLOAD_PORT flash monitor
```

`DOWNLOAD_PORT` 替换为下载串口。烧录前备份已有配置；不要点击整片擦除。更换源码所在目录时，使用干净源码，不携带旧 `build/` 和 `sdkconfig`。

- [使用说明](docs/USAGE.md)
- [最低硬件配置](docs/HARDWARE.md)
- [第三方依赖](THIRD_PARTY.md)
- [非商业源码许可](LICENSE)
