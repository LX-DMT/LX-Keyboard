#!/usr/bin/env python3
"""Convert PNG (RGBA) to LVGL 8 lv_img_dsc_t .c (TRUE_COLOR_ALPHA).

Default output matches LV_COLOR_DEPTH=16: RGB565 little-endian + 8-bit alpha (3 bytes/pixel).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("pip install pillow", file=sys.stderr)
    raise


def rgba_to_px16(r: int, g: int, b: int, a: int) -> tuple[int, int, int]:
    if a == 0:
        return (0, 0, 0)
    c16 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (c16 & 0xFF, (c16 >> 8) & 0xFF, a)


def rgba_to_px32(r: int, g: int, b: int, a: int) -> tuple[int, int, int, int]:
    return (b, g, r, a)


def png_to_c(
    png_path: Path,
    out_path: Path,
    symbol: str,
    size: int = 128,
    depth: int = 16,
) -> None:
    im = Image.open(png_path).convert("RGBA")
    if im.size != (size, size):
        im = im.resize((size, size), Image.Resampling.LANCZOS)
    w, h = im.size
    px = im.load()
    data = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if depth == 16:
                data.extend(rgba_to_px16(r, g, b, a))
            elif depth == 32:
                data.extend(rgba_to_px32(r, g, b, a))
            else:
                raise ValueError(f"unsupported depth {depth}")

    chunk_bytes = 12 if depth == 16 else 12
    lines = []
    for i in range(0, len(data), chunk_bytes):
        chunk = data[i : i + chunk_bytes]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

    upper = symbol.upper()
    body = "\n".join(lines)
    text = f'''#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{upper}
#define LV_ATTRIBUTE_IMG_{upper}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{upper} uint8_t {symbol}_map[] = {{
{body}
}};

const lv_img_dsc_t {symbol} = {{
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.w = {w},
  .header.h = {h},
  .data_size = sizeof({symbol}_map),
  .data = {symbol}_map,
}};
'''
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8")
    bpp = 3 if depth == 16 else 4
    print(f"Wrote {out_path} ({w}x{h}, depth={depth}, {len(data)} bytes, {bpp} B/px)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("png")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-n", "--name", required=True, help="C symbol e.g. alpha_menu_cal_pic")
    ap.add_argument("--size", type=int, default=128)
    ap.add_argument("--depth", type=int, default=16, choices=(16, 32))
    args = ap.parse_args()
    png_to_c(Path(args.png), Path(args.output), args.name, args.size, args.depth)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
