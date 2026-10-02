#!/usr/bin/env python3
"""Generates src/components/themes/bookshelf/BookshelfWallpaper.h, the celestial
wallpaper tile behind the Bookshelf home (an original design: a rayed sun, a
small zodiac wheel, a crescent moon in a dotted orbit, moon phases, a ringed
planet, constellations and scattered stars).

Every shape is drawn at each wrapped offset, so the tile repeats without seams.
Lines are drawn at 5x and downscaled to 1 bit; the sun's disk gets a light
ordered-dither fill. The result is about 6% ink: a pale pattern that titles and
books stay readable over.

Usage: python3 scripts/generate_bookshelf_wallpaper.py [preview.png]
Requires Pillow.
"""
import math, random, sys
from PIL import Image, ImageDraw

W, H = 160, 192          # tile (final px); tiles seamlessly
S = 5                    # supersample
ink = Image.new("L", (W*S, H*S), 255)   # 0 = ink line
fillm = Image.new("L", (W*S, H*S), 0)   # 255 = dithered fill area
di = ImageDraw.Draw(ink)
df = ImageDraw.Draw(fillm)
LW = max(1, int(1.1*S))

def wrap(fn):
    """Draw at every wrapped offset so the tile repeats without seams."""
    for ox in (-W, 0, W):
        for oy in (-H, 0, H):
            fn(ox, oy)

def P(x, y, ox=0, oy=0): return ((x+ox)*S, (y+oy)*S)

def circle(cx, cy, r, width=LW, dotted=False, fill=False):
    def f(ox, oy):
        if fill:
            df.ellipse([P(cx-r, cy-r, ox, oy), P(cx+r, cy+r, ox, oy)], fill=255)
        if dotted:
            n = max(12, int(2*math.pi*r/3))
            for i in range(n):
                a = 2*math.pi*i/n
                x, y = cx + r*math.cos(a), cy + r*math.sin(a)
                di.ellipse([P(x-0.5, y-0.5, ox, oy), P(x+0.5, y+0.5, ox, oy)], fill=0)
        else:
            di.ellipse([P(cx-r, cy-r, ox, oy), P(cx+r, cy+r, ox, oy)], outline=0, width=width)
    wrap(f)

def line(x0, y0, x1, y1, width=LW):
    wrap(lambda ox, oy: di.line([P(x0, y0, ox, oy), P(x1, y1, ox, oy)], fill=0, width=width))

def dot(x, y, r):
    wrap(lambda ox, oy: di.ellipse([P(x-r, y-r, ox, oy), P(x+r, y+r, ox, oy)], fill=0))

def crescent(cx, cy, r, shift, solid=True):
    """A crescent: disk minus an offset disk."""
    def f(ox, oy):
        target = di if solid else df
        if solid:
            di.ellipse([P(cx-r, cy-r, ox, oy), P(cx+r, cy+r, ox, oy)], fill=0)
            di.ellipse([P(cx-r+shift, cy-r-shift*0.3, ox, oy), P(cx+r+shift, cy+r-shift*0.3, ox, oy)], fill=255)
    wrap(f)

def star4(cx, cy, r, w=0.22):
    pts = []
    for i in range(8):
        a = math.pi/4*i - math.pi/2
        rr = r if i % 2 == 0 else r*w
        pts.append((cx + rr*math.cos(a), cy + rr*math.sin(a)))
    wrap(lambda ox, oy: di.polygon([P(x, y, ox, oy) for x, y in pts], fill=0))

def burst(cx, cy, r0, r1, n):
    for i in range(n):
        a = 2*math.pi*i/n
        line(cx + r0*math.cos(a), cy + r0*math.sin(a), cx + r1*math.cos(a), cy + r1*math.sin(a), max(1, int(0.8*S)))

random.seed(7)
# --- sun with rays, upper left
circle(34, 40, 11, fill=True)
circle(34, 40, 11)
circle(34, 40, 15, dotted=True)
burst(34, 40, 18, 30, 24)
# --- big crescent moon in an orbit, lower right
circle(112, 126, 26, dotted=True)
circle(112, 126, 19)
crescent(112, 126, 13, 6)
# --- zodiac-ish wheel, small, upper right
circle(120, 36, 18)
circle(120, 36, 11)
for i in range(12):
    a = 2*math.pi*i/12
    line(120 + 11*math.cos(a), 36 + 11*math.sin(a), 120 + 18*math.cos(a), 36 + 18*math.sin(a), max(1, int(0.8*S)))
star4(120, 36, 6)
# --- moon phases row, middle left
for i, x in enumerate((14, 30, 46)):
    circle(x, 100, 5)
    if i == 0: crescent(x, 100, 5, 3)
    if i == 1:
        wrap(lambda ox, oy, x=x: di.pieslice([P(x-5, 95, ox, oy), P(x+5, 105, ox, oy)], 90, 270, fill=0))
    if i == 2: dot(x, 100, 5)
# --- planet with a ring
# ring behind, the planet's body over it, then the ring's front half on top
wrap(lambda ox, oy: di.ellipse([P(42, 156, ox, oy), P(74, 166, ox, oy)], outline=0, width=LW))
wrap(lambda ox, oy: di.ellipse([P(51, 152, ox, oy), P(65, 166, ox, oy)], fill=255, outline=0, width=LW))
wrap(lambda ox, oy: di.arc([P(42, 156, ox, oy), P(74, 166, ox, oy)], 0, 180, fill=0, width=LW))
# --- constellations: dots joined by thin lines
for pts in ([(70, 70), (84, 62), (96, 74), (90, 90), (104, 96)],
            [(10, 140), (22, 128), (30, 146), (16, 170)],
            [(132, 168), (146, 156), (154, 176)]):
    for (a, b) in zip(pts, pts[1:]):
        line(a[0], a[1], b[0], b[1], max(1, int(0.7*S)))
    for x, y in pts:
        dot(x, y, 1.6)
# --- scattered sparkles and dust
for (x, y, r) in [(80, 20, 5), (150, 92, 4), (8, 66, 3.5), (86, 118, 4), (140, 6, 3), (66, 128, 3), (150, 140, 3.5), (100, 178, 3), (52, 74, 2.5), (126, 80, 2.5)]:
    star4(x, y, r)
for _ in range(38):
    dot(random.uniform(0, W), random.uniform(0, H), random.choice((0.6, 0.6, 0.9)))

small_ink = ink.resize((W, H), Image.LANCZOS)
small_fill = fillm.resize((W, H), Image.LANCZOS)
bayer = [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]]
out = Image.new("1", (W, H), 1)
px = out.load(); ip = small_ink.load(); fp = small_fill.load()
for y in range(H):
    for x in range(W):
        if ip[x, y] < 140:
            px[x, y] = 0
        elif fp[x, y] > 127 and bayer[y % 4][x % 4] < 6:
            px[x, y] = 0


if len(sys.argv) > 1:
    prev = Image.new("1", (W*3, H*2), 1)
    for i in range(3):
        for j in range(2):
            prev.paste(out, (i*W, j*H))
    prev.resize((W*6, H*4), Image.NEAREST).save(sys.argv[1])

import pathlib
rowBytes = (W + 7) // 8
lines = ["#pragma once", "", "#include <cstdint>", "",
         "// Celestial wallpaper tile for the Bookshelf home, 1 bit per pixel, rows of",
         f"// {rowBytes} bytes, most significant bit first, 1 = ink. Generated by",
         "// scripts/generate_bookshelf_wallpaper.py - do not edit by hand.",
         "namespace bookshelf {",
         f"constexpr int WALLPAPER_TILE_W = {W};",
         f"constexpr int WALLPAPER_TILE_H = {H};",
         f"constexpr uint8_t WALLPAPER_TILE[{rowBytes * H}] = {{"]
for yy in range(H):
    row = []
    for b in range(rowBytes):
        v = 0
        for bit in range(8):
            xx = b*8 + bit
            if xx < W and px[xx, yy] == 0:
                v |= 0x80 >> bit
        row.append(f"0x{v:02x}")
    lines.append("    " + ", ".join(row) + ",")
lines += ["};", "}  // namespace bookshelf", ""]
target = pathlib.Path(__file__).resolve().parent.parent / "src/components/themes/bookshelf/BookshelfWallpaper.h"
target.write_text("\n".join(lines))
print(f"wrote {target}")
