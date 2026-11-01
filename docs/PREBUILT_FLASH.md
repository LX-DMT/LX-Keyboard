# 免编译烧录：键盘、旋钮、接收器

使用预编译固件不需要安装 ESP-IDF，也不需要编译源码。ESP32-S3 使用 **`.bin` 固件**，本项目提供的是合并镜像，不是 `.hex`。

## 选择固件

下载并完整解压 `LX-Firmware-v1.0.0.zip`，每块设备只选择对应的一份文件：

| 设备 | 固件文件 | 烧录地址 | 最低 Flash / PSRAM |
| --- | --- | --- | --- |
| 键盘 | `keyboard-factory.bin` | `0x0` | 16 MiB / 8 MiB |
| 旋钮 | `knob-factory.bin` | `0x0` | 16 MiB / 8 MiB |
| 接收器 | `receiver-factory.bin` | `0x0` | 8 MiB / 8 MiB |

三者均为 ESP32-S3、ESP-IDF 5.5.6，Flash / PSRAM 为 80 MHz。键盘、接收器使用四线 Flash 和八线 PSRAM（3.3 V），旋钮使用八线 Flash 和八线 PSRAM（1.8 V）。外围电路和 GPIO 必须与本项目一致，三份固件不可互刷。

合并镜像已包含引导程序、分区表、初始 OTA 数据和应用；键盘镜像还包含语音模型。**所有合并文件都写入 `0x0`，不要按应用分区地址写入，也不要另外叠加其他 BIN。**

## 1. 连接下载接口

- 每次只处理一块设备，保持稳定供电，关闭串口监视器和上位机。
- 键盘使用独立下载连接；旋钮使用 UART0 下载连接；接收器可使用 UART 下载连接，或进入 ROM 下载模式后的原生 USB。
- UART 下载器必须使用 **3.3 V 逻辑电平**，TX/RX 交叉连接并共地。不可将 5 V 串口电平接到芯片引脚。
- 有自动下载电路时，由下载器控制 BOOT/EN；没有时，将 GPIO0 拉低，复位或重新上电，进入下载模式后释放 GPIO0。有 BOOT/RESET 按键的板可按住 BOOT，再按一下 RESET。
- 没有 BOOT 按键且没有自动下载电路，需要按板上预留的 GPIO0/EN 下载接口接线。**产品 USB 配置串口不是 ROM 下载口。**

在 Windows 设备管理器中通过拔插下载器确认 COM 号。原生 USB 进入下载模式后可能出现新的端口，需重新选择。旋钮由键盘供电时，避免键盘休眠断电。

## 2. 使用乐鑫图形工具烧录（Windows）

1. 从[乐鑫官方下载工具页面](https://www.espressif.com/en/support/download/other-tools)下载并解压 **Flash Download Tools**，运行其中的程序。
2. 启动选项选择 **ESP32-S3**、**Develop**、**UART**；这里的 UART 下载连接也可使用设备已进入 ROM 下载模式后的虚拟串口。
3. 在固件列表第一行选择对应的 `*-factory.bin`，地址填写 **`0x0`**，勾选该行；其余文件行取消勾选。
4. 选择下载 COM 口。BAUD 建议从 **115200** 开始，连接稳定后可用 460800。
5. 若有 **DoNotChgBin** 选项，勾选以保留镜像内参数。若必须填写 SPI 参数，使用 **DIO / 80 MHz**，容量键盘和旋钮为 **16 MB**、接收器为 **8 MB**。旋钮镜像头同样使用 DIO，八线 Flash 由固件驱动处理，不要自行改写镜像头。
6. **确认这是新板或允许恢复出厂后，点击 ERASE，等待完成。** 再点击 **START**，等待显示 FINISH / 成功；过程中不要断电。
7. 释放 BOOT/GPIO0，复位或重新上电。若仍停在下载模式，拔掉下载连接后重新开机。

**ERASE 会清除 Wi-Fi、小智凭据、配对、灯光/按键设置及旋钮校准等数据。** 此包是首次烧录/恢复出厂包；已有设备需要保留设置时，不使用此流程，改用源码包中带完整备份的升级流程。合并镜像中的空隙也会覆盖配置区，不能仅通过跳过 ERASE 来保留配置。

不同工具版本的按钮名称略有区别，以芯片型号、文件地址和下载端口为准。下载工具本身不随固件包分发。

## 3. 烧录后首次使用

1. 旋钮按屏幕提示完成首次校准，确认旋转和按压正常。
2. 键盘通过旋钮选 USB 模式，连接产品 USB，测试实体按键及 H1 鼠标。
3. 打开配套 Windows 上位机，进入“配网”；需要接收器时同时插入接收器，完成配网和绑定。
4. 在“小智”页开始配置，按播报的六位验证码到[小智官网](https://xiaozhi.me/)绑定；已绑定的账号关系以官网为准，擦除本地 Flash 不会替你解除云端绑定。
5. 测试蓝牙、接收器 Wi-Fi 和 ESP-NOW 模式。接收器连接后状态灯应呼吸。

## 可选：用 esptool 命令行烧录

只需 Python 和 esptool，仍不需要 ESP-IDF。下面固定使用 esptool 4.11.0 的命令格式，在固件包目录运行：

```sh
python -m pip install esptool==4.11.0
python -m esptool --chip esp32s3 --port DOWNLOAD_PORT erase_flash
python -m esptool --chip esp32s3 --port DOWNLOAD_PORT --baud 115200 write_flash 0x0 keyboard-factory.bin
python -m esptool --chip esp32s3 --port DOWNLOAD_PORT verify_flash 0x0 keyboard-factory.bin
```

把 `DOWNLOAD_PORT` 换成下载串口；烧录旋钮或接收器时，将最后两条命令中的文件名换成对应文件。`erase_flash` 同样清空全部配置。

如果没有自动复位电路，每条命令前手动进入下载模式，并在 `--port DOWNLOAD_PORT` 后加入 `--before no_reset --after no_reset`，完成后手动复位。安全启动或 Flash 加密设备不适用本通用镜像流程。

## 常见问题

- **Connecting 超时**：确认是下载口、未被其他程序占用，并重新进入 ROM 下载模式。
- **串口突然消失**：检查供电和数据线；原生 USB 下载/运行端口可能不同。
- **烧录成功但不能启动**：检查固件是否选错、Flash/PSRAM 接口与电压是否匹配，以及 GPIO0 是否仍被拉低。
- **无语音或旋钮需要重新设置**：首次烧录需完成联网、官方绑定和旋钮校准；固件不包含他人的账号及设备配置。

固件包附带 SHA-256 清单；Windows 可用 `Get-FileHash .\keyboard-factory.bin -Algorithm SHA256` 与 `SHA256SUMS.txt` 对照。
