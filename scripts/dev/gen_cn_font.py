#!/usr/bin/env python3
"""Generate the 3DS bottom-screen Chinese glyph atlases.

Two faces are baked, matching the ALttP-3DS-CN project's bottom screen:

  body   Fusion Pixel 12px, regular -- menu text, values, hints
  large  GNU Unifont   16px, bold  -- panel titles and tab labels

A cell is 16x16, 2bpp (0 = transparent, 1 = outline, 2 = body), four pixels per
byte, 64 bytes per glyph. The outline is baked into the cell, so the renderer no
longer dilates at runtime. Each face is inset so its ink centre lands on cell
pixel (7, 7).

The character set is discovered by scanning the bottom-screen sources for
printable ASCII plus CJK codepoints, so adding a Chinese string only needs a
re-run -- the old hand-maintained table is what let '更' go missing.

Usage:
    gen_cn_font.py [--check]

    --check  report whether the headers are up to date; write nothing.

Requires Python 3 and Pillow, and the two OFL fonts next to this script.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOURCES = [
    "port/port_second_screen.c",
    "port/port_second_screen_theme.c",
    "platform/3ds/source/port_config_3ds.c",
    "platform/3ds/source/main_3ds.c",
    "port/port_config.h",
    "platform/3ds/source/config_values_3ds.h",
    # 3DS-only overlay (the idle-screen BACK button lives here).
    "platform/3ds/source/port_second_screen_3ds.c",
    # Update panel and the updater's status lines.
    "platform/3ds/source/update_ui_3ds.inc",
    "platform/3ds/source/updater.c",
]

FONT_DIR = os.path.join(ROOT, "port", "cn_fonts")
OUT_HEADER = os.path.join(ROOT, "port", "port_second_screen_cn_font.h")

CELL = 16
BODY = 2
OUTLINE = 1
CJK_ADVANCE = 16

# key, font file, px size, x inset, y inset, bold
FACES = [
    ("body", "fusion-pixel-12px-monospaced-zh_hans.bitmap.ttf", 12, 2, 1, False),
    ("large", "unifont-18.0.01.otf", 16, 0, 0, True),
]


def is_cjk(ch):
    return ("\u4e00" <= ch <= "\u9fff" or "\u3000" <= ch <= "\u303f"
            or "\uff00" <= ch <= "\uffef")


def used_chars():
    seen = set(chr(c) for c in range(0x20, 0x7F))
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            seen.update(ch for ch in fh.read() if is_cjk(ch))
    return sorted(seen, key=ord)


def bold_right_overlap(img):
    """Synthesize bold by shifting right, unioning, then reopening counters.

    Unifont ships one weight. Naive dilation fills enclosed holes (图, 备) so
    the last step subtracts the added material shifted once more.
    """
    from PIL import Image

    def shift(src, dx):
        return src.transform(src.size, Image.AFFINE, (1, 0, -dx, 0, 1, 0),
                             resample=Image.NEAREST)

    def union(a, b):
        return Image.frombytes("1", a.size,
                               bytes(x | y for x, y in zip(a.tobytes(), b.tobytes())))

    def subtract(a, b):
        return Image.frombytes("1", a.size,
                               bytes((x & ~y) & 0xFF for x, y in zip(a.tobytes(), b.tobytes())))

    solid = union(shift(img, 1), img)
    shadow = shift(subtract(solid, img), 1)
    return subtract(solid, shadow)


def render(font, off_x, off_y, ch, bold):
    from PIL import Image, ImageDraw
    img = Image.new("L", (CELL, CELL), 0)
    ImageDraw.Draw(img).text((off_x, off_y), ch, font=font, fill=255)
    if bold:
        mask = img.point(lambda v: 255 if v >= 128 else 0).convert("1")
        img = bold_right_overlap(mask).convert("L").point(lambda v: 255 if v else 0)
    return img


def pack(img):
    px = img.load()
    out = bytearray(CELL * CELL // 4)
    for y in range(CELL):
        for x in range(CELL):
            v = px[x, y]
            if v <= 0:
                continue
            level = BODY if v >= 128 else OUTLINE
            out[y * 4 + x // 4] |= level << ((3 - (x % 4)) * 2)
    return bytes(out)


def advance_of(img, ch):
    if is_cjk(ch):
        return CJK_ADVANCE
    bbox = img.getbbox()
    if bbox is None:
        return 4
    return max(3, bbox[2] - bbox[0] + 1)


def build_face(key, filename, size, off_x, off_y, bold, chars):
    from PIL import ImageFont
    path = os.path.join(FONT_DIR, filename)
    if not os.path.exists(path):
        sys.exit(f"missing font: {path}")
    font = ImageFont.truetype(path, size)
    cells, adv, glyphs = [], [], []
    ink_top, ink_bottom = CELL, -1
    for ch in chars:
        img = render(font, off_x, off_y, ch, bold)
        cells.append(ord(ch))
        adv.append(advance_of(img, ch))
        glyphs.append(pack(img))
        box = img.getbbox()
        if box:
            ink_top = min(ink_top, box[1])
            ink_bottom = max(ink_bottom, box[3] - 1)
    if ink_bottom < ink_top:
        ink_top, ink_bottom = 0, CELL - 1
    return cells, adv, b"".join(glyphs), ink_top, ink_bottom


def emit(face_key, cells, adv, glyphs, ink_top, ink_bottom, size, bold, out):
    import re
    prefix = "kTmcCn" if face_key == "body" else "kTmcCnLarge"
    count = f"TMC_CN_COUNT" if face_key == "body" else "TMC_CN_LARGE_COUNT"
    note = ("Fusion Pixel 12px, regular" if face_key == "body"
            else "GNU Unifont 16px, synthesized bold")
    ink_macro = "TMC_CN_INK" if face_key == "body" else "TMC_CN_LARGE_INK"
    out.append(f"/* {note}: {len(cells)} cells, ink rows {ink_top}..{ink_bottom}. */")
    out.append(f"#define {ink_macro}_TOP {ink_top}")
    out.append(f"#define {ink_macro}_BOTTOM {ink_bottom}")
    out.append(f"#define {count} {len(cells)}")
    out.append(f"static const uint32_t {prefix}Cell[{count}] = {{")
    for i in range(0, len(cells), 10):
        out.append("  " + ", ".join(f"0x{c:04X}u" for c in cells[i:i + 10]) + ",")
    out.append("};")
    out.append(f"static const uint8_t {prefix}Advance[{count}] = {{")
    for i in range(0, len(adv), 20):
        out.append("  " + ", ".join(str(a) for a in adv[i:i + 20]) + ",")
    out.append("};")
    out.append(f"static const uint8_t {prefix}Glyph[{count} * 64] = {{")
    per_line = 16
    for gi in range(len(cells)):
        block = glyphs[gi * 64:(gi + 1) * 64]
        for i in range(0, 64, per_line):
            out.append("  " + ", ".join(f"0x{b:02X}" for b in block[i:i + per_line]) + ",")
    out.append("};")
    out.append("")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    chars = used_chars()
    if not chars:
        sys.exit("no characters found")

    body = build_face(*FACES[0], chars)
    large = build_face(*FACES[1], chars)

    out = []
    out.append("// Bottom-screen Simplified-Chinese glyph atlas for the 3DS port.")
    out.append("// 16x16 2bpp cells (0 = transparent, 1 = outline, 2 = body), 64 bytes per")
    out.append("// glyph. Two faces, both inset so the ink centre lands on cell pixel (7,7):")
    out.append("//   body   Fusion Pixel 12px, regular        -- menu text")
    out.append("//   large  GNU Unifont 16px, synthesized bold -- panel titles and tabs")
    out.append("//")
    out.append("// Fonts: Fusion Pixel Font (SIL OFL 1.1), GNU Unifont (SIL OFL 1.1 /")
    out.append("// GPLv2+ font exception). See FONT-NOTICE.md.")
    out.append("//")
    out.append("// Generated by scripts/dev/gen_cn_font.py -- do not edit by hand.")
    out.append("#pragma once")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define TMC_CN_PX 16")
    out.append("")
    emit(FACES[0][0], *body, FACES[0][2], FACES[0][4], out)
    emit(FACES[1][0], *large, FACES[1][2], FACES[1][4], out)
    text = "\n".join(out)

    if args.check:
        current = open(OUT_HEADER, encoding="utf-8").read() if os.path.exists(OUT_HEADER) else ""
        if current != text:
            print("port_second_screen_cn_font.h is stale; re-run gen_cn_font.py")
            return 1
        print(f"atlas OK ({len(chars)} cells per face)")
        return 0

    with open(OUT_HEADER, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"wrote {OUT_HEADER}: {len(chars)} cells per face, "
          f"{len(chars)*64*2} bytes of glyph data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
