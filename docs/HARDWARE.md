# 硬件与接线

[返回首页](../README.md) · [最低硬件配置](../HARDWARE.md)

引脚定义以源码为准。本包不含原理图、PCB、BOM 和机械文件。

## 键盘

定义：[board.h](../firmware/keyboard/main/board.h)，电平和初始化：[board.c](../firmware/keyboard/main/board.c)。

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

## 旋钮

定义：[board_pins.h](../firmware/knob/components/board/include/board_pins.h)。

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

## 接收器

LED1 为 3V3 经限流电阻和 LED 接 GPIO13，低电平亮；当前板限流电阻为 1 kΩ。固件使用 2 kHz PWM，连接呼吸峰值约 25%。

产品 USB 提供 HID 和配置 CDC；烧录使用 UART 或 ROM 下载模式下的原生 USB。

## 分区与容量

分区表是实际依据：[键盘](../firmware/keyboard/partitions.csv)、[旋钮](../firmware/knob/partitions.csv)、[接收器](../firmware/receiver/partitions.csv)。

修改分区、存储接口、电压或 GPIO 后需重新验证。
