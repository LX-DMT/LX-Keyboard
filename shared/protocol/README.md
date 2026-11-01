# LX v2 公共协议

本目录包含 C / Python 帧编解码、CRC 与流解析。当前类型表、链路及配置边界见[协议说明](../../docs/PROTOCOLS.md)。

- [头文件](include/lx_protocol.h)
- [C 实现](lx_protocol.c)
- [Python 实现](lx_protocol.py)
- [灯光设置协议](LIGHTING.md)

旋钮 UART 单独使用 [LX v1](../../firmware/knob/docs/host_link/proto.md)。本版不支持上位机代打或 Wi-Fi 直连电脑输入，相关类型编号仅保留。

协议模拟检查：在仓库根目录执行 `python -m unittest discover -s tests -p "test_protocol.py"`。
