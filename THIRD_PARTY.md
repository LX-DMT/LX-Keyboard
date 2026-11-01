# 固定第三方组件

上位机依赖单独列在 [pc_app/THIRD_PARTY.md](pc_app/THIRD_PARTY.md)。

版本以组件声明为索引，实际内容以 `third_party.lock.json` 的逐文件 SHA-256 为准。共 21 个组件，保留各自来源元数据和许可证。语音相关组件含模型/预编译库，不将其描述为全部可重建源码。

| 目录 | 声明版本 | 许可文件 |
| --- | --- | --- |
| esp_codec_dev | 1.5.9 | [LICENSE](third_party/esp_codec_dev/LICENSE) |
| esp_tinyusb | 1.7.6~2 | [LICENSE](third_party/esp_tinyusb/LICENSE) |
| esp_websocket_client | 1.8.0 | [LICENSE](third_party/esp_websocket_client/LICENSE) |
| espressif__arduino-foc | 2.3.0~3 | [LICENSE](third_party/espressif__arduino-foc/LICENSE) |
| espressif__cjson | 1.7.19~2 | [LICENSE](third_party/espressif__cjson/LICENSE) |
| espressif__cmake_utilities | 0.5.3 | [license.txt](third_party/espressif__cmake_utilities/license.txt) |
| espressif__dl_fft | 0.6.0 | 清单声明 MIT；完整许可待补核 |
| espressif__esp-dl | 3.3.11 | [LICENSE](third_party/espressif__esp-dl/LICENSE) |
| espressif__esp-dsp | 1.8.0 | [LICENSE](third_party/espressif__esp-dsp/LICENSE) |
| espressif__esp-sr | 2.5.3 | [LICENSE](third_party/espressif__esp-sr/LICENSE) |
| espressif__esp_audio_codec | 2.6.2 | [LICENSE](third_party/espressif__esp_audio_codec/LICENSE) |
| espressif__esp_lcd_gc9a01 | 2.0.4 | [license.txt](third_party/espressif__esp_lcd_gc9a01/license.txt) |
| espressif__esp_new_jpeg | 1.0.2 | [LICENSE](third_party/espressif__esp_new_jpeg/LICENSE) |
| espressif__esp_simplefoc | 1.3.0 | [license.txt](third_party/espressif__esp_simplefoc/license.txt) |
| espressif__i2c_bus | 1.5.2 | [license.txt](third_party/espressif__i2c_bus/license.txt) |
| espressif__iqmath | 1.11.0~1 | [LICENSE](third_party/espressif__iqmath/LICENSE) |
| espressif__led_strip | 2.0.0 | [LICENSE](third_party/espressif__led_strip/LICENSE) |
| led_strip | 2.5.5 | [LICENSE](third_party/led_strip/LICENSE) |
| lvgl__lvgl | 8.3.11 | [LICENCE.txt](third_party/lvgl__lvgl/LICENCE.txt) |
| tinyusb | 0.21.0~1 | [LICENSE](third_party/tinyusb/LICENSE) |
| usb_host_hid | 1.2.0 | [LICENSE](third_party/usb_host_hid/LICENSE) |

LVGL 使用本项目的 `LV_CPICKER_DEF_QF=3` 配置。第三方快照含本地修改，升级依赖时需验证并更新清单。

cJSON、其 Unity 测试子目录和 TinyUSB 的 `.gitattributes` 关闭换行转换及 export-ignore，以保证 Git 克隆和源码归档与依赖锁文件一致。

ESP-SR 与 ESP Audio Codec 等组件的许可包含 Espressif 硬件限制。项目的非商业条款只适用于自有部分，不替代第三方许可证。dl_fft 清单声明 MIT，但当前快照缺少独立许可文本；发布前应按该版本上游确认补齐。素材权属和再分发检查见[许可说明](docs/LICENSING.md)。
