# 键盘第三方依赖

首次编译由 ESP-IDF Component Manager 从官方组件仓库下载，版本固定如下，包摘要见 `dependencies.lock`。下载组件保留各自许可，不适用本项目非商业许可。

| 组件 | 版本 |
| --- | --- |
| [espressif/esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.5.9) | 1.5.9 |
| [espressif/esp_tinyusb](https://components.espressif.com/components/espressif/esp_tinyusb/versions/1.7.6~2) | 1.7.6~2 |
| [espressif/tinyusb](https://components.espressif.com/components/espressif/tinyusb/versions/0.21.0~1) | 0.21.0~1 |
| [espressif/usb_host_hid](https://components.espressif.com/components/espressif/usb_host_hid/versions/1.2.0) | 1.2.0 |
| [espressif/led_strip](https://components.espressif.com/components/espressif/led_strip/versions/2.5.5) | 2.5.5 |
| [espressif/esp-sr](https://components.espressif.com/components/espressif/esp-sr/versions/2.5.3) | 2.5.3 |
| [espressif/esp-dl](https://components.espressif.com/components/espressif/esp-dl/versions/3.3.11) | 3.3.11 |
| [espressif/esp-dsp](https://components.espressif.com/components/espressif/esp-dsp/versions/1.8.0) | 1.8.0 |
| [espressif/dl_fft](https://components.espressif.com/components/espressif/dl_fft/versions/0.6.0) | 0.6.0 |
| [espressif/cjson](https://components.espressif.com/components/espressif/cjson/versions/1.7.19~2) | 1.7.19~2 |
| [espressif/esp_new_jpeg](https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2) | 1.0.2 |
| [espressif/esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2) | 2.6.2 |

本地保留 `components/esp_websocket_client`，来源为 Espressif esp-protocols 提交 `730838fac89064b4460be0ece1f799978afc50df`，声明版本 1.8.0，附 Apache-2.0 许可。语音组件含受 Espressif 许可限制的模型和预编译库；dl_fft 0.6.0 的独立许可文本仍需上游确认。

素材来源说明见 [许可说明](docs/LICENSING.md)。
