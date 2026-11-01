#!/usr/bin/env python3
"""Convert LVGL 8 lv_img_dsc_t headers to LVGL 9 lv_image_dsc_t (.c files)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def extract_depth32_section(text: str) -> str | None:
    marker = "#if LV_COLOR_DEPTH == 32"
    start = text.find(marker)
    if start < 0:
        return None
    end = text.find("#endif", start)
    if end < 0:
        return None
    return text[start + len(marker) : end].strip()


def parse_descriptor(text: str) -> tuple[str, int, int, int]:
    m = re.search(r"const\s+lv_img_dsc_t\s+(\w+)\s*=", text)
    if not m:
        raise ValueError("Could not find lv_img_dsc_t name")
    name = m.group(1)

    hdr = re.search(
        r"LV_IMG_CF_TRUE_COLOR_ALPHA,\s*\d+,\s*\d+,\s*(\d+),\s*(\d+)",
        text,
    )
    if not hdr:
        raise ValueError("Could not parse image header")
    w, h = int(hdr.group(1)), int(hdr.group(2))

    size_m = re.search(r"(\d+)\s*\*\s*LV_IMG_PX_SIZE_ALPHA_BYTE", text)
    data_size = int(size_m.group(1)) * 4 if size_m else w * h * 4
    return name, w, h, data_size


def convert_file(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    section = extract_depth32_section(text)
    if section is None:
        raise ValueError(f"{src}: no LV_COLOR_DEPTH == 32 section")

    name, w, h, data_size = parse_descriptor(text)
    map_name = f"{name}_map"
    stride = w * 4

    out = f'''#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{name.upper()}
#define LV_ATTRIBUTE_IMG_{name.upper()}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{name.upper()} uint8_t {map_name}[] = {{
{section}
}};

const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {stride},
    .data_size = sizeof({map_name}),
    .data = {map_name},
}};
'''
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(out, encoding="utf-8")
    print(f"Converted {src.name} -> {dst}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="LVGL8 .h image files")
    parser.add_argument("-o", "--out-dir", required=True, help="Output directory")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    for inp in args.inputs:
        src = Path(inp)
        dst = out_dir / (src.stem + ".c")
        convert_file(src, dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
