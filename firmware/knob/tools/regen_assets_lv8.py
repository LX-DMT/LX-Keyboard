#!/usr/bin/env python3
"""Regenerate components/ui/assets/*.c from src/img/*.h (LVGL8 native 16-bit TRUE_COLOR_ALPHA)."""

from __future__ import annotations

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "components", "ui", "assets")
SRC_IMG = os.path.join(ROOT, "..", "src", "img")


def extract_section(text: str, marker: str) -> str | None:
    idx = text.find(marker)
    if idx < 0:
        return None
    start = idx + len(marker)
    end = text.find("#endif", start)
    if end < 0:
        return None
    return text[start:end].strip()


def parse_h_descriptor(text: str) -> tuple[str, int, int] | None:
    m = re.search(
        r"const lv_img_dsc_t (\w+) = \{[\s\S]*?"
        r"LV_IMG_CF_TRUE_COLOR_ALPHA,\s*0,\s*0,\s*(\d+),\s*(\d+)",
        text,
    )
    if not m:
        return None
    return m.group(1), int(m.group(2)), int(m.group(3))


def rgb888_to_rgb565_bytes(b: int, g: int, r: int) -> tuple[int, int]:
    """LVGL map stores B,G,R byte order in ARGB8888 export; output LE RGB565."""
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return c & 0xFF, (c >> 8) & 0xFF


def convert_argb8888_map_to_lv8(data4: list[int]) -> list[int]:
    out: list[int] = []
    for i in range(0, len(data4), 4):
        b, g, r, a = data4[i : i + 4]
        lo, hi = rgb888_to_rgb565_bytes(b, g, r)
        out.extend((lo, hi, a))
    return out


def format_map_lines(data: list[int], indent: str = "  ") -> list[str]:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    if lines:
        lines[-1] = lines[-1].rstrip(",")
    return lines


def build_c(name: str, map_data: list[int], w: int, h: int) -> str:
    attr = f"LV_ATTRIBUTE_IMG_{name.upper()}"
    lines = format_map_lines(map_data)
    body = "\n".join(lines)
    return f'''#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {attr}
#define {attr}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {attr} uint8_t {name}_map[] = {{
{body}
}};

const lv_img_dsc_t {name} = {{
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.w = {w},
  .header.h = {h},
  .data_size = sizeof({name}_map),
  .data = {name}_map,
}};
'''


def parse_map_bytes(section: str) -> list[int]:
    # Match 0xdb, 0x14, 0x20 or 0xdb,0x14,0x20
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", section)]


def regen_from_h(c_path: str, h_path: str) -> bool:
    base = os.path.splitext(os.path.basename(c_path))[0]
    with open(h_path, "r", encoding="utf-8") as f:
        htext = f.read()

    section = extract_section(htext, "#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0")
    if not section:
        print(f"FAIL {base}: no LV_COLOR_DEPTH==16 section in .h")
        return False

    desc = parse_h_descriptor(htext)
    if not desc:
        print(f"FAIL {base}: no lv_img_dsc_t in .h")
        return False
    name, w, h = desc
    map_data = parse_map_bytes(section)
    if len(map_data) != w * h * 3:
        print(f"FAIL {base}: expected {w*h*3} bytes, got {len(map_data)}")
        return False

    out = build_c(name, map_data, w, h)
    with open(c_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print(f"OK {base} ({w}x{h}, {len(map_data)} bytes)")
    return True


def regen_argb8888_c(c_path: str) -> bool:
    """Convert remaining 4-byte-per-pixel .c assets (no src/img .h)."""
    base = os.path.splitext(os.path.basename(c_path))[0]
    with open(c_path, "r", encoding="utf-8") as f:
        text = f.read()

    m = re.search(
        r"const lv_img_dsc_t (\w+) = \{[\s\S]*?\.header\.w = (\d+),[\s\S]*?\.header\.h = (\d+)",
        text,
    )
    if not m:
        print(f"FAIL {base}: cannot parse descriptor")
        return False
    name, w, h = m.group(1), int(m.group(2)), int(m.group(3))

    map_m = re.search(rf"{name}_map\[\] = \{{([\s\S]*?)\}};", text)
    if not map_m:
        print(f"FAIL {base}: no map array")
        return False
    raw = parse_map_bytes(map_m.group(1))
    expected3 = w * h * 3
    expected4 = w * h * 4
    if len(raw) == expected3:
        map_data = raw
    elif len(raw) == expected4:
        map_data = convert_argb8888_map_to_lv8(raw)
    else:
        print(f"FAIL {base}: map {len(raw)} bytes, expected {expected3} or {expected4}")
        return False
    out = build_c(name, map_data, w, h)
    with open(c_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print(f"OK {base} (converted ARGB8888 -> LV8 16-bit, {len(map_data)} bytes)")
    return True


def main() -> int:
    ok = fail = 0
    for c_path in sorted(glob.glob(os.path.join(ASSETS, "*.c"))):
        base = os.path.splitext(os.path.basename(c_path))[0]
        h_path = os.path.join(SRC_IMG, base + ".h")
        if not os.path.isfile(h_path):
            if regen_argb8888_c(c_path):
                ok += 1
            else:
                fail += 1
            continue

        if regen_from_h(c_path, h_path):
            ok += 1
        else:
            fail += 1

    print(f"\nRegenerated {ok} assets, {fail} failed")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())