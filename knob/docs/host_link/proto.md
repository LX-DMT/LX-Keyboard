# LX Knob Host Link Protocol (v1)

Binary frame format over UART0 @ 115200 (GPIO43 TX / GPIO44 RX).

## Frame layout

| Offset | Field | Value |
|--------|-------|-------|
| 0 | SOF0 | `0x4C` ('L') |
| 1 | SOF1 | `0x58` ('X') |
| 2 | Version | `0x01` |
| 3 | Type | see below |
| 4 | Payload length | 0–32 |
| 5..4+N | Payload | |
| 5+N | CRC8 | XOR polynomial 0x07 over bytes [2..4+N] |

## Message types (device → host)

| Type | Name | Payload |
|------|------|---------|
| `0x10` | REMOTE_LINK | `[cmd, arg0, arg1]` |
| `0x20` | REMOTE_LED_COLOR | `[cmd, h, s, v]` |
| `0x21` | REMOTE_LED_MODE | `[cmd, arg0, arg1]` |
| `0x30` | REMOTE_AUDIO | `[cmd, arg0, arg1]` |
| `0x40` | DEVICE_STATUS | optional |

## Message types (host → device)

| Type | Name | Payload |
|------|------|---------|
| `0x80` | HOST_STATS | `[cpu%, gpu%, ram%]` |
| `0x81` | HEARTBEAT | 预留（host_link.h 已定义，旋钮 RX 未解析） |
| `0x82` | POWER_STATS | `[bus_mv: u16 LE][current_ma: i16 LE]` — bus mV, INA219 raw signed mA; knob Power UI inverts so **+ = charge** (pale green), **− = discharge** (pale red) |
| `0x83` | POWER_PREVIEW | `[state: u8]` 1=按下预览 Power 屏, 0=松开恢复原界面 |
| `0x84` | AUDIO_STATUS | `[sound_on: u8]` 1=有声, 0=静音（与键盘 NVS `param->mute` 一致） |

## Remote command bytes (`cmd`)

| Value | Command |
|-------|---------|
| 0 | USB link |
| 1 | BLE link |
| 2 | WiFi link |
| 3 | ESP-NOW link |
| 4 | LED HSV |
| 5 | LED breath |
| 6 | LED rhythm |
| 7 | Audio mute |
| 8 | Audio unmute |

## Legacy Arduino mapping (reference)

Old frames used `0xAA 0x55` … `0x55 0xAA`. New protocol is not byte-compatible.

| Old type | New mapping |
|----------|-------------|
| 0x11 USB | REMOTE_LINK cmd=0 |
| 0x12 BLE | REMOTE_LINK cmd=1 |
| 0x13 WiFi | REMOTE_LINK cmd=2 |
| 0x14 ESP-NOW | REMOTE_LINK cmd=3 |
| 0x21 HSV | REMOTE_LED_COLOR |
| 0x22 breath | REMOTE_LED_MODE cmd=5 |
| 0x23 rhythm | REMOTE_LED_MODE cmd=6 |
| 0x31 audio | REMOTE_AUDIO |

## CRC8 algorithm

```python
def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc
```
