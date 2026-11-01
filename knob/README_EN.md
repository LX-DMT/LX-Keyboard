# LX knob firmware

Standalone ESP32-S3 project for ESP-IDF **5.5.6**.

Open this folder directly in VS Code, select your installed ESP-IDF 5.5.6 environment and click **Build Project**. Pinned dependencies are downloaded automatically on the first build. No other repository folders or preparation scripts are required.

In an activated ESP-IDF terminal, run `idf.py build`, then `idf.py -p DOWNLOAD_PORT flash monitor`. Select the actual download port and back up existing settings before flashing.

- [Usage](docs/USAGE.md)
- [Hardware](docs/HARDWARE.md)
- [UART protocol](docs/host_link/proto.md)
- [Dependencies](THIRD_PARTY.md)
- [Noncommercial source license](LICENSE)
