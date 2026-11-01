#!/usr/bin/env python3
"""Minimal host tool for LX Knob UART protocol v1."""

import argparse
import math
import sys
import time

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

SOF0 = 0x4C
SOF1 = 0x58
VERSION = 0x01
MAX_PAYLOAD = 32

HOST_MSG_HOST_STATS = 0x80

MSG_NAMES = {
    0x10: "REMOTE_LINK",
    0x20: "REMOTE_LED_COLOR",
    0x21: "REMOTE_LED_MODE",
    0x30: "REMOTE_AUDIO",
    0x40: "DEVICE_STATUS",
    0x80: "HOST_STATS",
}


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def build_frame(msg_type: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    header = bytes([VERSION, msg_type, len(payload)]) + payload
    return bytes([SOF0, SOF1]) + header + bytes([crc8(header)])


def parse_frames(buf: bytearray):
    """Extract all complete LX frames from buffer, resyncing on SOF."""
    i = 0
    while i + 5 <= len(buf):
        if not (buf[i] == SOF0 and buf[i + 1] == SOF1):
            i += 1
            continue
        plen = buf[i + 4]
        if plen > MAX_PAYLOAD:
            i += 1
            continue
        total = 5 + plen + 1
        if i + total > len(buf):
            break
        frame = bytes(buf[i:i + total])
        body = frame[2:-1]
        if crc8(body) == frame[-1]:
            yield frame[3], frame[5:5 + plen]
            i += total
        else:
            i += 1
    del buf[:i]


def format_frame(msg_type: int, payload: bytes) -> str:
    name = MSG_NAMES.get(msg_type, "UNKNOWN")
    return f"type=0x{msg_type:02X} ({name}) payload={payload.hex()}"


def cmd_monitor(args):
    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    buf = bytearray()
    count = 0
    print(f"Monitoring {args.port} @ {args.baud} (Ctrl+C to stop)")
    print("Device sends a frame on each UI action (HSV change, Connect button, etc.).")
    print("Test: LED -> Color -> rotate knob on HSV sliders while this runs.")
    try:
        while True:
            chunk = ser.read(512)
            if chunk:
                buf.extend(chunk)
                for msg_type, payload in parse_frames(buf):
                    count += 1
                    print(f"[{count}] RX {format_frame(msg_type, payload)}")
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print(f"Total frames: {count}")


def clamp_pct(value: float) -> int:
    return max(0, min(100, int(round(value))))


def send_stats_frame(ser, cpu: int, gpu: int, ram: int) -> bytes:
    payload = bytes([cpu & 0xFF, gpu & 0xFF, ram & 0xFF])
    frame = build_frame(HOST_MSG_HOST_STATS, payload)
    ser.write(frame)
    ser.flush()
    return payload


def cmd_send_stats(args):
    ser = serial.Serial(args.port, args.baud, timeout=1)
    sent = 0
    try:
        while True:
            payload = send_stats_frame(ser, args.cpu, args.gpu, args.ram)
            sent += 1
            print(f"[{sent}] TX {format_frame(HOST_MSG_HOST_STATS, payload)}")
            if args.interval <= 0:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


SWEEP_KEYS = (0, 1, 9, 10, 50, 93, 99, 100)


def cmd_sweep_stats(args):
    """Cycle key CPU/GPU/RAM values including 100% for UI boundary testing."""
    ser = serial.Serial(args.port, args.baud, timeout=1)
    sent = 0
    t0 = time.time()
    print(f"Sweep stats on {args.port} @ {args.baud} every {args.interval}s (Ctrl+C to stop)")
    print("On device: Standby -> Instrument (仪表盘), keep this script running.")
    try:
        while True:
            if args.duration > 0 and time.time() - t0 >= args.duration:
                break
            key = SWEEP_KEYS[sent % len(SWEEP_KEYS)]
            payload = send_stats_frame(ser, key, key, key)
            sent += 1
            print(f"[{sent}] cpu={key:3d}% gpu={key:3d}% ram={key:3d}%")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print(f"Sent {sent} frames")


def cmd_demo_stats(args):
    """Sweep CPU/GPU/RAM with different phases so all three rings move."""
    ser = serial.Serial(args.port, args.baud, timeout=1)
    sent = 0
    t0 = time.time()
    print(f"Demo stats on {args.port} @ {args.baud} every {args.interval}s (Ctrl+C to stop)")
    print("On device: Standby -> Instrument (仪表盘), keep this script running.")
    try:
        while True:
            elapsed = time.time() - t0
            if args.duration > 0 and elapsed >= args.duration:
                break
            cpu = clamp_pct(50 + 50 * math.sin(elapsed * 1.2))
            gpu = clamp_pct(50 + 50 * math.sin(elapsed * 0.85 + 2.1))
            ram = clamp_pct(50 + 50 * math.sin(elapsed * 1.55 + 4.2))
            payload = send_stats_frame(ser, cpu, gpu, ram)
            sent += 1
            print(f"[{sent}] cpu={cpu:3d}% gpu={gpu:3d}% ram={ram:3d}%")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print(f"Sent {sent} frames")


def main():
    parser = argparse.ArgumentParser(
        description="LX Knob host UART tool",
        epilog=(
            "Examples:\n"
            "  host_link_tool.py monitor COM10\n"
            "  host_link_tool.py stats COM10 --cpu 80 --gpu 40 --ram 60\n"
            "  host_link_tool.py demo COM10 --interval 0.4\n"
            "  host_link_tool.py sweep COM10 --interval 1.0"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("-p", "--port", default=None, help="Serial port (required, or pass after subcommand)")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    sub = parser.add_subparsers(dest="cmd", required=True)

    monitor = sub.add_parser("monitor", help="Print frames sent by device")
    monitor.add_argument("port_pos", nargs="?", help="Serial port (alternative to -p)")

    stats = sub.add_parser("stats", help="Send CPU/GPU/RAM stats to device")
    stats.add_argument("port_pos", nargs="?", help="Serial port (alternative to -p)")
    stats.add_argument("--cpu", type=int, default=42)
    stats.add_argument("--gpu", type=int, default=30)
    stats.add_argument("--ram", type=int, default=55)
    stats.add_argument(
        "--interval",
        type=float,
        default=0,
        help="Repeat every N seconds (0 = send once)",
    )

    demo = sub.add_parser("demo", help="Animate CPU/GPU/RAM 0-100 (three different waveforms)")
    demo.add_argument("port_pos", nargs="?", help="Serial port (alternative to -p)")
    demo.add_argument("--interval", type=float, default=0.4, help="Seconds between updates")
    demo.add_argument("--duration", type=float, default=0, help="Stop after N seconds (0 = until Ctrl+C)")

    sweep = sub.add_parser("sweep", help="Cycle key values 0-100 including 100% for UI testing")
    sweep.add_argument("port_pos", nargs="?", help="Serial port (alternative to -p)")
    sweep.add_argument("--interval", type=float, default=1.0, help="Seconds between updates")
    sweep.add_argument("--duration", type=float, default=0, help="Stop after N seconds (0 = until Ctrl+C)")

    args = parser.parse_args()
    args.port = args.port or args.port_pos
    if not args.port:
        parser.error("Specify the actual device port using -p or the positional port argument")
    if args.cmd == "monitor":
        cmd_monitor(args)
    elif args.cmd == "stats":
        cmd_send_stats(args)
    elif args.cmd == "demo":
        cmd_demo_stats(args)
    elif args.cmd == "sweep":
        cmd_sweep_stats(args)


if __name__ == "__main__":
    main()
