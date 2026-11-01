# 蓝星键盘上位机

## 使用说明

Windows 10/11 x64：完整解压 `LXKeyboardSetup-v1.0.0-Windows-x64.zip`，运行 `LXKeyboardSetup.exe`。`_internal` 必须与 EXE 一起保留，无需安装 Python。

设备配置前，将键盘切到 **USB 模式**，用数据线连接键盘的产品 USB 接口。

| 页面 | 操作 |
| --- | --- |
| 配网 | 点击“开始配网”，选择设备并填写 2.4 GHz Wi-Fi；绑定接收器时同时插入接收器，按向导验证与保存 |
| 小智 | 点击“开始配置”，按键盘播报的六位验证码到[官方控制台](https://xiaozhi.me/)绑定；更换账号先在官网解除原绑定 |
| 按键 | 参照方向键位置图，为两个可自定义按键选择功能并保存；“读取键盘”可查看当前设置 |
| 灯光 | 选择颜色、常亮/呼吸/律动/关闭效果及亮度，点击“保存灯效”；无需先读取，低电量自动关闭灯光开关独立保存 |

“旋钮同步”显示电脑 CPU、GPU 和内存负载，可自动或手动选择 USB、蓝牙、接收器 Wi-Fi、接收器 ESP-NOW。该选择不改变键盘的输入模式；GPU 数据缺失时旋钮按 0 显示。

关闭窗口后继续在托盘运行，右键托盘选择“退出”才完全退出。打开“开机自启动”后，登录 Windows 时自动在托盘运行；移动程序目录后，关闭再开启此开关以更新启动路径。

语音输入由键盘发送按键，在目标输入框切到微软拼音中文状态后使用；上位机不代替键盘输入文字。

## 源码运行和打包

安装 Windows x64 Python 3.13（含 Tcl/Tk），在本目录执行：

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt -r requirements-build.txt
.venv\Scripts\python main.py
.venv\Scripts\python -m PyInstaller --noconfirm LXKeyboardSetup.spec
```

输出目录为 `dist/LXKeyboardSetup/`。保留仓库的 `shared/protocol/`，源码通过相对路径加载协议。首次安装 Python 依赖需要联网；不需要 ESP-IDF 或额外 C 编译器。

自有部分按根目录 [LICENSE](../LICENSE) 非商业许可提供；第三方依赖见 [THIRD_PARTY.md](THIRD_PARTY.md)。
