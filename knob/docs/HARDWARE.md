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


定义：[board_pins.h](../components/board/include/board_pins.h)。

| 外设 | GPIO / 参数 |
| --- | --- |
| 电机六路 PWM UH / UL / VH / VL / WH / WL | 15 / 9 / 17 / 5 / 8 / 6 |
| AS5047 编码器 MOSI / MISO / SCLK / CS | 11 / 13 / 12 / 10，SPI2 |
| GC9A01 屏 MOSI / SCLK / DC / RST | 3 / 2 / 21 / 16，SPI3，240×240 |
| HX711 DOUT / SCK | 40 / 39 |
| WS2812 DATA / PWR | 18 / 14，50 颗 |
| 电池采样 | 7，10 kΩ + 10 kΩ 分压 |
| UART0 TX / RX | 43 / 44，115200，8N1 |
| RF 使能 | 45 |

编码器按 AS5047 接线，源码读角类名为 `AS5048a`。装配完成后再校准。

键盘 TX14 → 旋钮 RX44，旋钮 TX43 → 键盘 RX21，并共地。UART 电平：3.3 V。
