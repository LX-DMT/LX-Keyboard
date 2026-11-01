# LX knob firmware

ESP32-S3 haptic dial with a 240×240 GC9A01 display, LVGL 8.3.11 and ESP-IDF **5.5.6**.

This source-available release permits noncommercial use of original project code only; see [LICENSE](../../LICENSE). Third-party terms remain in force. This is not an OSI-approved open-source license.

Build from the repository root in an activated ESP-IDF 5.5.6 terminal:

```sh
python scripts/build.py knob
```

Keep the entire repository, including vendored dependencies. Flash using the backup-aware helper described in the [build guide](../../docs/BUILD_FLASH.md). Existing calibration is preserved; a blank device requires first-time strain and motor calibration. See the [Chinese usage guide](../../docs/KNOB.md), [hardware](../../docs/HARDWARE.md) and [UART protocol](docs/host_link/proto.md).
