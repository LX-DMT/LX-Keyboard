# 蓝星键盘 · 旋钮 · 接收器 v1.0.0

## 项目功能

| 部分 | 功能 |
| --- | --- |
| 键盘 | USB、蓝牙、接收器 Wi-Fi、接收器 ESP-NOW 四种模式；H1 鼠标转发；小智语音、语音输入；两个自定义按键；灯光、电量及休眠管理 |
| 旋钮 | 圆屏显示、旋转与按压、触觉反馈；切换连接模式、灯效和音频设置；显示电量和电脑负载 |
| 接收器 | Wi-Fi / ESP-NOW 无线输入转 USB 键盘、鼠标及媒体控制；状态指示灯 |
| Windows 上位机 | 配网与接收器绑定、小智官方激活、按键与灯光配置、电脑负载监视及旋钮同步；托盘运行、开机自启动 |

## 使用说明

下载：[Windows 上位机](https://github.com/LX-DMT/LX-Keyboard/releases/download/v1.0.0/LXKeyboardSetup-v1.0.0-Windows-x64.zip) · [三板预编译固件](https://github.com/LX-DMT/LX-Keyboard/releases/download/v1.0.0/LX-Firmware-v1.0.0.zip) · [完整源码包](https://github.com/LX-DMT/LX-Keyboard/archive/refs/heads/main.zip)

1. 通过旋钮选择连接模式。USB 模式连接键盘产品 USB；蓝牙模式在电脑上配对；接收器模式需先完成绑定。
2. 将 Windows 上位机程序包完整解压，运行 `LXKeyboardSetup.exe`，保留同目录的 `_internal` 文件夹。
3. 配网、配置小智、按键和灯光时，将键盘切到 **USB 模式**并通过产品 USB 连接电脑；绑定接收器时同时插入接收器。
4. 在“配网”页设置 2.4 GHz Wi-Fi；在“小智”页开始配置，按播报的六位验证码到[小智官网](https://xiaozhi.me/)绑定。
5. “旋钮同步”支持自动或手动选择 USB、蓝牙、接收器 Wi-Fi、接收器 ESP-NOW 通道。关闭窗口后继续在托盘运行；托盘菜单“退出”才完全退出。

详细说明：[上位机](pc_app/README.md) · [键盘](docs/KEYBOARD.md) · [旋钮](docs/KNOB.md) · [接收器](docs/RECEIVER.md) · [首次使用](docs/QUICK_START.md)

## 源码编译烧录指南

### 直接烧录预编译固件

无需搭建编译环境：下载 `LX-Firmware-v1.0.0.zip`，用乐鑫 Flash Download Tools 选择 ESP32-S3，把对应的 `*-factory.bin` 烧录到 **`0x0`**。

此流程用于首次烧录或恢复出厂，会清除原配置。文件选择、下载接线和图形工具操作见[免编译烧录步骤](docs/PREBUILT_FLASH.md)。

### 固件

安装并打开 **ESP-IDF 5.5.6** 环境终端。在 VS Code 中直接打开 `keyboard`、`knob` 或 `receiver` 文件夹，点击 ESP-IDF 扩展的“编译项目”；首次编译自动联网下载固定版本依赖。每个工程均可单独复制使用。

需要一次编译三块板时，在仓库根目录执行：

```sh
python scripts/verify_portability.py
python scripts/build.py all
```

将 `DOWNLOAD_PORT` 替换为对应设备的下载串口，分别编译并烧录：

```sh
python scripts/build.py keyboard --flash --port DOWNLOAD_PORT
python scripts/build.py knob --flash --port DOWNLOAD_PORT
python scripts/build.py receiver --flash --port DOWNLOAD_PORT
```

Windows 也可运行 `build-flash.cmd keyboard DOWNLOAD_PORT`。脚本先备份，再写入和校验；产品 USB 配置口与下载口需区分。详见[编译与烧录](docs/BUILD_FLASH.md)。

### 上位机

Windows x64 安装 **Python 3.13**，在 `pc_app` 目录执行：

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt -r requirements-build.txt
.venv\Scripts\python main.py
.venv\Scripts\python -m PyInstaller --noconfirm LXKeyboardSetup.spec
```

程序生成在 `pc_app/dist/LXKeyboardSetup/`。上位机编译独立于 ESP-IDF，详见[上位机说明](pc_app/README.md)。

## 最低硬件配置

三块设备均使用 **ESP32-S3**，Flash / PSRAM 运行频率均为 **80 MHz**。

| 部分 | Flash 不低于 | PSRAM 不低于 | 存储接口 |
| --- | --- | --- | --- |
| 键盘 | 16 MiB | 8 MiB | 四线 Flash（DIO）、八线 PSRAM，3.3 V |
| 旋钮 | 16 MiB | 8 MiB | 八线 Flash、八线 PSRAM，1.8 V |
| 接收器 | 8 MiB | 8 MiB | 四线 Flash（DIO）、八线 PSRAM，3.3 V |

GPIO 与外围接线见[硬件说明](docs/HARDWARE.md)。上位机面向 Windows 10/11 x64；蓝牙同步需要电脑具备 BLE 功能。
