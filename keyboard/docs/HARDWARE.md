# 最低硬件配置

ESP32-S3；Flash / PSRAM：**80 MHz**。容量单位：MiB。

| 部分 | Flash 不低于 | PSRAM 不低于 | 存储接口 |
| --- | --- | --- | --- |
| 键盘 | **16 MB** | **8 MB** | 四线 Flash（DIO 运行）、八线 PSRAM，3.3 V |
| 旋钮 | **16 MB** | **8 MB** | 八线 Flash、八线 PSRAM，1.8 V |
| 接收器 | **8 MB** | **8 MB** | 四线 Flash（DIO 运行）、八线 PSRAM，3.3 V |

### 官方资料

- ESP32-S3-WROOM-1 / WROOM-1U 数据手册
- ESP32-S3-WROOM-2 数据手册
- ESP32-S3-PICO-1 数据手册
- ESP-IDF 5.5.6 Flash / PSRAM 配置关系

## 接收器状态灯

GPIO13，低电平点亮，限流电阻 1 kΩ。

| 状态 | 灯效 |
| --- | --- |
| 应用开始运行 | 亮300毫秒 |
| USB已就绪，等待键盘 | 每2秒亮100毫秒 |
| 接收器Wi-Fi或ESP-NOW收到有效数据/心跳 | 3秒一轮柔和呼吸，峰值约25% PWM占空比 |
| USB未就绪或已报告初始化失败 | 亮250毫秒、灭250毫秒循环 |

## 接线


定义：[board.h](../main/board.h)，电平和初始化：[board.c](../main/board.c)。

| 信号 | GPIO |
| --- | --- |
| 主电源 / 旋钮供电控制 | 45 / 39 |
| USB 模式控制 / SD 使能 | 4 / 6（SD 低有效，USB 模式低，其他模式高） |
| Fn | 9 |
| 扫描 PL / CE / CLK / MISO | 3 / 7 / 12 / 13 |
| 旋钮 UART TX / RX | 14 / 21 |
| RGB 电源 / 数据 / 侧灯 | 5 / 38 / 42 |
| 音频关闭控制 | 1 |
| 音频 I²C SDA / SCL | 8 / 18 |
| I²S MCLK / BCLK / WS / DOUT / DIN | 2 / 17 / 47 / 15 / 16 |
| 电源监测 I²C SDA / SCL | 41 / 40 |

接口连接以板卡丝印和电路为准。
