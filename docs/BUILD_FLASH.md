# 编译与烧录

[返回首页](../README.md)

不修改源码、只想首次烧录的用户，可直接使用[预编译 BIN 和图形工具](PREBUILT_FLASH.md)，不需要安装 ESP-IDF。该流程恢复出厂；本页的备份升级流程用于保留已有设备配置。

上位机的运行和打包步骤见[上位机说明](../pc_app/README.md)，使用独立的 Python 环境。

## 环境要求

- **ESP-IDF v5.5.6**，含其子模块、ESP32-S3 编译工具和官方约束安装的 Python 依赖。
- Git、足够的磁盘空间及 USB 下载驱动；在 Windows 使用 ESP-IDF 5.5.6 环境终端。
- 三套工程目标均为 `esp32s3`，版本检查会拒绝其他 IDF 版本。
- 三套工程的Flash频率均明确为80 MHz，PSRAM仍为Octal 80 MHz；最低容量与接口要求见[硬件说明](../HARDWARE.md)。

SDK 安装参见 [ESP-IDF v5.5.6 官方入门文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/get-started/index.html)。只需安装 5.5.6 对应工具。不要将下载的单个 SDK 源码目录误当作已经安装好的工具环境。

Linux/macOS 可按官方方法安装并激活 `export.sh`。默认使用 Windows 构建和烧录；其他系统需自行验证工具链和设备兼容性。

## VS Code 一键编译

1. 安装乐鑫 **ESP-IDF** 扩展，配置完整的 **5.5.6** 开发环境。
2. “文件 → 打开文件夹”，直接选择 `keyboard`、`knob` 或 `receiver`，不要选择上层仓库目录。
3. 点击 ESP-IDF 扩展的 **Build Project / 编译项目** 按钮。首次构建会自动下载依赖，无需先运行脚本或手工复制组件。
4. 烧录时选择该设备的下载串口，再点击扩展的 **Flash Device / 烧录**。

工程已指定 ESP32-S3 和默认硬件参数。IDE 配置不包含本机 SDK 路径或串口号；首次使用仍需在扩展中选择自己安装的 5.5.6 环境。原生扩展烧录不做整片备份，需要备份校验时使用下文脚本。

## 命令行编译

`keyboard/`、`knob/`、`receiver/` 都是独立 ESP-IDF 工程。可以只解压所需工程，不依赖其他板子的目录。首次构建需要联网，组件管理器按清单和锁文件下载固定版本依赖（含语音模型与库）。

在根目录执行：

```sh
idf.py --version
python scripts/verify_portability.py
python scripts/build.py all
```

第一条应显示 ESP-IDF v5.5.6；校验脚本检查工程内依赖路径、分区容量和保留组件 SHA-256 和工程独立性；最后一条依次编译三套工程。日志保存在 `artifacts/logs/`，终端在每套完成时报告结果。首次构建可能较慢。

单独编译：

```sh
python scripts/build.py keyboard
python scripts/build.py knob
python scripts/build.py receiver
```

若 `IDF_PATH` 未设置，可加 `--idf SDK_DIRECTORY`，但仍需使用已安装 IDF Python 依赖的环境。各工程也支持进入 `keyboard`、`knob` 或 `receiver` 后运行 `idf.py build`。不需要重复执行 `set-target`；目标由 `sdkconfig.defaults` 指定。

切换 SDK 或搬动目录后，请使用新的干净源码副本构建，避免带入原 `build/`、`sdkconfig` 或 `managed_components/`；保留随源码提供的 `dependencies.lock`。不修改全局 Git 配置，不依赖用户本机 IDE 设置。

## 识别接口

| 设备 | 产品/运行接口 | 下载接口 |
| --- | --- | --- |
| 键盘 | 产品 USB 提供 HID 和配置 CDC；需切 USB 模式 | 独立下载连接，用于 ROM 下载和诊断日志 |
| 旋钮 | UART 与键盘收发控制协议 | UART0 下载；与日志/控制复用，注意不要多方同时驱动 |
| 接收器 | 插电脑后的 USB HID / CDC | UART 下载，或硬件进入 ROM 下载后新枚举的原生 USB 端口 |

Windows 可在设备管理器查看，或运行：

```sh
python -m serial.tools.list_ports -v
```

通过拔插对应下载器确认身份。文档中的 `DOWNLOAD_PORT`、`KEYBOARD_PORT` 和 `RESET_PORT` 都是占位符，必须换成自己的端口；Windows 常见 `COMx`，Linux 常见 `/dev/ttyUSB0` 或 `/dev/ttyACM0`。多设备同时存在时逐个确认，不能照搬他人的 COM 号。

## 一键编译、备份、烧录

关闭占用下载串口的监视器，停止语音会话，保持供电，在根目录执行其中一条：

```sh
python scripts/build.py keyboard --flash --port DOWNLOAD_PORT
python scripts/build.py knob --flash --port DOWNLOAD_PORT
python scripts/build.py receiver --flash --port DOWNLOAD_PORT
```

Windows 等价入口：

```bat
build-flash.cmd keyboard DOWNLOAD_PORT
build-flash.cmd knob DOWNLOAD_PORT
build-flash.cmd receiver DOWNLOAD_PORT
```

每次只处理一块板，不支持 `all --flash`。正常流程为：

1. 用指定 SDK 编译。
2. 核对 ESP32-S3、Flash 容量、当前分区和应用名称；拒绝不兼容布局、错误板型和安全启动/加密设备。
3. 读取并校验整片 Flash 备份，包括实物超出软件配置的容量。
4. 写入需要更新的引导程序、分区/应用及键盘模型。镜像相同则跳过对应写入。
5. 校验写入镜像和所有未写区域，复位并尝试记录启动日志。

已有设备保留配置、配对、校准和 OTA 选择；空白设备初始化 OTA 数据。不执行整片擦除。实际输出位于 `artifacts/device-backups/`，缓存位于 `artifacts/flash-cache/`；它们包含私人设置，不进入 Git 或源码 ZIP。

可用 `--expect-mac DEVICE_MAC` 增加身份检查。用 `--backup-cache DIRECTORY` 指定备份缓存时，仍会用设备当前摘要核对缓存，不能跳过备份核验。

原生USB下载路径的备份使用至多4 KiB的读取块，避免部分Windows链路在大块读取时截断；每块及完整备份仍需通过摘要校验。失败时不会因已有旧备份而跳过本次数据核对。

### 旋钮供电保持

旋钮由键盘供电时，键盘休眠可能在编译或备份期间切断电源。可使用：

```sh
python scripts/build.py knob --flash --port KNOB_PORT --power-port KEYBOARD_PORT
```

`KEYBOARD_PORT` 是另一个可通信的键盘诊断串口，不能与旋钮串口相同，也不能正被其他程序占用。脚本发送保持唤醒请求；若无法保活则报错。独立稳定供电时无需该选项。

### 接收器原生 USB 下载

产品 CDC 不接受 ROM 烧录命令。若使用外接适配器控制 BOOT/EN 进入下载，而数据走原生 USB：

```sh
python scripts/build.py receiver --flash --port ROM_USB_PORT --control-port RESET_PORT
```

前提是已根据硬件连接确认复位控制有效，并知道进入下载后对应的 ROM USB 端口。脚本不会让任意普通 USB 口自动获得下载能力。没有 BOOT 按键且没有可用控制线时，先补齐下载连接，不反复对产品 CDC 尝试写入。

## 首次烧录与升级边界

一键脚本支持**全空白 Flash**与**同分区、同工程的已有设备**。若 Flash 有未知数据而没有匹配分区，或已有其他固件，则主动停止。这是保护行为，不应通过删除检查绕过。首次使用其他来源固件的板子，应先单独备份、确认板型和数据迁移方案，再由所有者决定是否清空；本指南不默认执行恢复出厂。

使用原生 `idf.py flash` 会绕过本项目的完整备份与保护区校验，不作为本指南的默认升级步骤。不要手工猜偏移烧录单个 bin；镜像和偏移以本次生成的 `flasher_args.json` 为准。

## 烧录后的确认

- bootloader 和 app 都报告 v5.5.6；没有 panic、反复重启或 PSRAM 初始化错误。
- 键盘可切换模式并正常实体输入；旋钮校准、旋转和按压正常。
- 接收器恢复产品 USB 枚举，已绑定模式可以打字和转发鼠标。
- 原设置保留；新设备按快速开始完成校准和配置。

`boot.log` 能收到启动信息不等于所有功能验收通过。原生 USB 从下载模式切回产品接口时端口可能消失/变化，应另行检查设备枚举。

## 打包本地源码

```sh
python scripts/package_source.py
```

生成 `artifacts/releases/` 下 ZIP 和 SHA-256 文件清单；排除构建缓存、设备备份和运行数据，保留必要的第三方二进制资源。提交前执行[发布检查表](RELEASE_CHECKLIST.md)。

完成三板构建后，维护者可在 ESP-IDF 5.5.6 环境生成免编译工厂包：

```sh
python scripts/check_release.py --builds
python scripts/package_firmware.py --output artifacts/releases/LX-Firmware-v1.0.0
```

输出目录必须尚不存在。脚本只读取构建产物并校验合并结果，不连接设备；工厂包用于首次烧录或恢复出厂。
