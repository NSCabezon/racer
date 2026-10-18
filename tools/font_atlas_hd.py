#!/usr/bin/env python3
"""Redraw the game's bitmap font atlases at 4x with a vector typeface.

Real-ESRGAN invents letter shapes on these ~7 px glyphs (broken N, M, G, 9),
so the HD pack excludes them (textures/exclude.txt) and this script makes the
replacements instead. Each glyph is found in the original atlas (rows by
horizontal projection, glyphs by vertical projection inside a row), then the
same character is rendered from a TTF and stretched onto the glyph's ink box,
so the game's texture rectangles still frame it. Glyphs with no typeface
equivalent (marked '.' in LAYOUTS), and rows whose glyph count doesn't match,
fall back to a contour rebuild: bicubic 4x plus a smoothstep around 50%
intensity. The italic HUD digits use the contour rebuild throughout.

Atlases are I4 (intensity, RGB = alpha). Output: same, at SCALE.

Usage: python3 tools/font_atlas_hd.py tex_dump hd_fonts [--font PATH] [--sheet out.png]
"""
import argparse, os
from PIL import Image, ImageDraw, ImageFont

SCALE = 4
FONT = "/System/Library/Fonts/Supplemental/Arial Black.ttf"

# Rows of characters as they appear in each atlas, top to bottom. '.' = keep
# the contour rebuild for that glyph. None = contour rebuild for the atlas.
LAYOUTS = {
    "7ee32ce753e56246": ["ABCDEF", "GHIJKLM", "NOPQRS", "TUVWXY", "Z:;-!?01",
                         "2345678", "9..\u00ab\u00bb", ".....", ".."],
    "e9e795d7b9978059": ["ABCDEFG", "OPQRSTU", ":;-!?()/", "HIJKLMN", "VWXYZ",
                         "1234567", "890\u2122\u00a9\u00ae", "..\u00ab\u00bb",
                         ".....", ".."],
    "2e245c4c9fa9f66a": None,
}

def contour(ch, k=SCALE, lo=0.38, hi=0.62):
    big = ch.resize((ch.size[0] * k, ch.size[1] * k), Image.BICUBIC)

    def f(v):
        t = min(1.0, max(0.0, (v / 255 - lo) / (hi - lo)))
        return int(255 * t * t * (3 - 2 * t))
    return big.point([f(i) for i in range(256)])


def runs(flags):
    """[(start, end)] of consecutive True values."""
    out, start = [], None
    for i, v in enumerate(flags + [False]):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append((start, i))
            start = None
    return out


def glyph_boxes(alpha):
    w, h = alpha.size
    px = alpha.load()
    rows = runs([any(px[x, y] > 0 for x in range(w)) for y in range(h)])
    out = []
    for y0, y1 in rows:
        cols = runs([any(px[x, y] > 0 for y in range(y0, y1)) for x in range(w)])
        boxes = []
        for x0, x1 in cols:
            # sub-pixel ink box: an edge row/column whose peak intensity is p
            # counts as covered by p (antialias fringe on 5-7 px glyphs)
            rmax = [max(px[x, y] for x in range(x0, x1)) / 255 for y in range(y0, y1)]
            cmax = [max(px[x, y] for y in range(y0, y1)) / 255 for x in range(x0, x1)]
            def edges(m, base):
                lit = [i for i, v in enumerate(m) if v > 0]
                a, b = lit[0], lit[-1]
                return base + a + 1 - m[a], base + b + m[b]
            ex0, ex1 = edges(cmax, x0)
            ey0, ey1 = edges(rmax, y0)
            boxes.append((ex0, ey0, ex1, ey1))
        out.append(((y0, y1), boxes))
    return out


def render_char(font, c, size):
    w, h = size
    if c == "I":
        # the game's I has slab serifs; a sans I stretched to its box is a block
        im = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(im)
        bar, stem = max(1, round(h * 0.2)), max(1, round(w * 0.42))
        d.rectangle((0, 0, w - 1, bar - 1), fill=255)
        d.rectangle((0, h - bar, w - 1, h - 1), fill=255)
        d.rectangle(((w - stem) // 2, 0, (w - stem) // 2 + stem - 1, h - 1), fill=255)
        return im
    im = Image.new("L", (600, 600), 0)
    ImageDraw.Draw(im).text((100, 100), c, font=font, fill=255)
    return im.crop(im.getbbox()).resize(size, Image.LANCZOS)


def redraw(alpha, layout, font):
    k = SCALE
    out = contour(alpha)
    if layout is None:
        return out
    rows = glyph_boxes(alpha)
    for r, ((y0, y1), boxes) in enumerate(rows):
        chars = layout[r] if r < len(layout) else ""
        if set(chars) <= {"."}:
            continue
        if len(chars) != len(boxes):
            print(f"  row {r}: {len(boxes)} glyphs found, layout has {len(chars)!r}; contour rebuild")
            continue
        # capitals and digits share one cap height per row (antialias fringe
        # makes single glyphs like C and S measure a pixel taller)
        caps = [b for c, b in zip(chars, boxes) if c.isalnum() and c not in "QJ"]
        if caps:
            top = sorted(b[1] for b in caps)[len(caps) // 2]
            bot = sorted(b[3] for b in caps)[len(caps) // 2]
        for c, (bx0, by0, bx1, by1) in zip(chars, boxes):
            if c == ".":
                continue
            if c.isalnum():
                by0, by1 = top, (by1 if c in "QJ" else bot)
            gx, gy = round(bx0 * k), round(by0 * k)
            g = render_char(font, c, (max(1, round(bx1 * k) - gx), max(1, round(by1 * k) - gy)))
            # clear the whole glyph column of this row, then place the vector glyph
            cx0, cx1 = int(bx0), min(int(bx1) + 1, alpha.size[0])
            px = alpha.load()
            while cx0 > 0 and any(px[cx0 - 1, y] > 0 for y in range(y0, y1)):
                cx0 -= 1
            while cx1 < alpha.size[0] and any(px[cx1, y] > 0 for y in range(y0, y1)):
                cx1 += 1
            out.paste(0, (cx0 * k, y0 * k, cx1 * k, y1 * k))
            out.paste(g, (gx, gy))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="texture dump dir (tex_dump)")
    ap.add_argument("dst", help="output dir for <hash>.v5.png")
    ap.add_argument("--font", default=FONT)
    ap.add_argument("--size", type=int, default=400)
    args = ap.parse_args()
    font = ImageFont.truetype(args.font, args.size)
    os.makedirs(args.dst, exist_ok=True)
    for h, layout in LAYOUTS.items():
        src = os.path.join(args.src, f"{h}.v5.png")
        if not os.path.exists(src):
            print(f"{h}: not dumped yet, skipped")
            continue
        print(h)
        a = Image.open(src).convert("RGBA").split()[3]
        big = redraw(a, layout, font)
        Image.merge("RGBA", (big, big, big, big)).save(os.path.join(args.dst, f"{h}.v5.png"))


if __name__ == "__main__":
    main()
