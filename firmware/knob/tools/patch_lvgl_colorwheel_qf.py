#!/usr/bin/env python3
"""Patch LVGL colorwheel for lx_knob: QF=3 + knob-only invalidation. Run after idf.py fullclean."""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(
    ROOT,
    "..", "..", "third_party",
    "lvgl__lvgl",
    "src",
    "extra",
    "widgets",
    "colorwheel",
    "lv_colorwheel.c",
)

QF_MARKER = "#define LV_CPICKER_DEF_QF"
QF_WANT = "#define LV_CPICKER_DEF_QF 3"

SET_HSV_OLD = """    if(colorwheel->hsv.h == hsv.h && colorwheel->hsv.s == hsv.s && colorwheel->hsv.v == hsv.v) return false;

    colorwheel->hsv = hsv;

    refr_knob_pos(obj);

    lv_obj_invalidate(obj);

    return true;"""

SET_HSV_NEW = """    if(colorwheel->hsv.h == hsv.h && colorwheel->hsv.s == hsv.s && colorwheel->hsv.v == hsv.v) return false;

    bool h_changed = colorwheel->hsv.h != hsv.h;
    bool s_changed = colorwheel->hsv.s != hsv.s;
    bool v_changed = colorwheel->hsv.v != hsv.v;

    colorwheel->hsv = hsv;

    refr_knob_pos(obj);

    bool ring_needs_redraw = false;
    switch(colorwheel->mode) {
    case LV_COLORWHEEL_MODE_HUE:
        ring_needs_redraw = s_changed || v_changed;
        break;
    case LV_COLORWHEEL_MODE_SATURATION:
        ring_needs_redraw = h_changed || v_changed;
        break;
    case LV_COLORWHEEL_MODE_VALUE:
        ring_needs_redraw = h_changed || s_changed;
        break;
    default:
        ring_needs_redraw = true;
        break;
    }

    if(ring_needs_redraw) {
        lv_obj_invalidate(obj);
    }
    else {
        invalidate_knob(obj);
    }

    return true;"""


def patch_qf(lines: list[str]) -> tuple[list[str], bool]:
    changed = False
    out = []
    for line in lines:
        if line.strip().startswith(QF_MARKER):
            if line.strip() != QF_WANT:
                out.append(QF_WANT)
                changed = True
            else:
                out.append(line)
        else:
            out.append(line)
    return out, changed


def patch_set_hsv(text: str) -> tuple[str, bool]:
    if SET_HSV_NEW in text:
        return text, False
    if SET_HSV_OLD not in text:
        print("WARN: lv_colorwheel_set_hsv block not found; skip knob invalidation patch")
        return text, False
    return text.replace(SET_HSV_OLD, SET_HSV_NEW, 1), True


def main() -> int:
    if not os.path.isfile(TARGET):
        print(f"SKIP: {TARGET} not found (run idf.py build first to fetch lvgl)")
        return 0

    with open(TARGET, "r", encoding="utf-8") as f:
        text = f.read()

    lines = text.splitlines()
    lines, qf_changed = patch_qf(lines)
    text = "\n".join(lines) + ("\n" if text.endswith("\n") else "")

    text, hsv_changed = patch_set_hsv(text)

    if not qf_changed and not hsv_changed:
        print("OK: colorwheel patches already applied")
        return 0

    with open(TARGET, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    if qf_changed:
        print(f"Patched QF -> {QF_WANT}")
    if hsv_changed:
        print("Patched lv_colorwheel_set_hsv -> knob-only invalidation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
