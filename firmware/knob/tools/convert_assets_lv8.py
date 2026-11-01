#!/usr/bin/env python3
import glob
import os
import re

assets_dir = os.path.join(os.path.dirname(__file__), "..", "components", "ui", "assets")
pat = re.compile(
    r"const lv_image_dsc_t (\w+) = \{\s*"
    r"\.header\.magic = LV_IMAGE_HEADER_MAGIC,\s*"
    r"\.header\.cf = LV_COLOR_FORMAT_ARGB8888,\s*"
    r"\.header\.flags = 0,\s*"
    r"\.header\.w = (\d+),\s*"
    r"\.header\.h = (\d+),\s*"
    r"\.header\.stride = \d+,\s*"
    r"\.data_size = sizeof\(\1_map\),\s*"
    r"\.data = \1_map,\s*\};",
    re.MULTILINE,
)

ok = 0
fail = 0
for path in sorted(glob.glob(os.path.join(assets_dir, "*.c"))):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if "lv_image_dsc_t" not in text:
        continue

    def repl(m):
        name, w, h = m.group(1), m.group(2), m.group(3)
        return (
            f"const lv_img_dsc_t {name} = {{\n"
            f"  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n"
            f"  .header.w = {w},\n"
            f"  .header.h = {h},\n"
            f"  .data_size = sizeof({name}_map),\n"
            f"  .data = {name}_map,\n"
            f"}};"
        )

    new_text, n = pat.subn(repl, text)
    if n != 1:
        print(f"FAIL {os.path.basename(path)} matches={n}")
        fail += 1
        continue
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(new_text)
    print(f"OK {os.path.basename(path)}")
    ok += 1

print(f"done ok={ok} fail={fail}")
raise SystemExit(1 if fail else 0)
