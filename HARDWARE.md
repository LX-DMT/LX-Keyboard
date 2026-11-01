# 最低硬件配置

ESP32-S3；Flash / PSRAM：**80 MHz**。容量单位：MiB。

| 部分 | Flash 不低于 | PSRAM 不低于 | 存储接口 |
| --- | --- | --- | --- |
| 键盘 | **16 MB** | **8 MB** | 四线 Flash（DIO 运行）、八线 PSRAM，3.3 V |
| 旋钮 | **16 MB** | **8 MB** | 八线 Flash、八线 PSRAM，1.8 V |
| 接收器 | **8 MB** | **8 MB** | 四线 Flash（DIO 运行）、八线 PSRAM，3.3 V |

### 官方资料

- [ESP32-S3-WROOM-1 / WROOM-1U 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP32-S3-WROOM-2 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-2_datasheet_en.pdf)
- [ESP32-S3-PICO-1 数据手册](https://documentation.espressif.com/esp32-s3-pico-1_datasheet_en.html)
- [ESP-IDF 5.5.6 Flash / PSRAM 配置关系](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-guides/flash_psram_config.html)

## 接收器状态灯

GPIO13，低电平点亮，限流电阻 1 kΩ。

| 状态 | 灯效 |
| --- | --- |
| 应用开始运行 | 亮300毫秒 |
| USB已就绪，等待键盘 | 每2秒亮100毫秒 |
| 接收器Wi-Fi或ESP-NOW收到有效数据/心跳 | 3秒一轮柔和呼吸，峰值约25% PWM占空比 |
| USB未就绪或已报告初始化失败 | 亮250毫秒、灭250毫秒循环 |
