#!/usr/bin/env python3
"""Create the grayscale Pokémon portraits used by the Pokémon sleep screens.

Input: PokeAPI Sprites' official artwork (RGBA PNGs named ``<id>.png``).
Output: ``<output>/sleep/NNN.bmp``, one per species - a SIZE x SIZE, 4-bit
indexed BMP. Palette entries 0..14 are evenly spaced grays (black to white);
entry 15 marks transparent pixels. The firmware composites these into the
sleep room and dithers the result to the panel's gray levels at runtime, so
the portraits keep 15 smooth levels instead of being dithered here.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

try:
    from PIL import Image, ImageFilter, ImageOps
except ImportError as error:  # pragma: no cover - environment-specific message
    raise SystemExit("Pillow is required: python -m pip install Pillow") from error

SIZE = 240
GRAY_LEVELS = 15
TRANSPARENT_INDEX = 15
ALPHA_THRESHOLD = 128


def palette() -> list[int]:
    entries: list[int] = []
    for index in range(GRAY_LEVELS):
        value = round(index * 255 / (GRAY_LEVELS - 1))
        entries += [value, value, value]
    entries += [255, 0, 255]  # transparent marker, never shown
    return entries


def convert(source: Path, destination: Path, size: int = SIZE) -> None:
    with Image.open(source) as opened:
        rgba = opened.convert("RGBA")
    # Trim the transparent margin, then fit inside the square, bottom-aligned
    # so every Pokémon "sits" on the same baseline in the room.
    bbox = rgba.getchannel("A").point(lambda a: 255 if a >= ALPHA_THRESHOLD else 0).getbbox()
    if bbox is None:
        raise ValueError(f"{source.name}: fully transparent")
    rgba = rgba.crop(bbox)
    scale = min(size / rgba.width, size / rgba.height)
    fitted = rgba.resize((max(1, round(rgba.width * scale)), max(1, round(rgba.height * scale))),
                         Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    canvas.alpha_composite(fitted, ((size - fitted.width) // 2, size - fitted.height))

    alpha = canvas.getchannel("A")
    gray = ImageOps.autocontrast(canvas.convert("RGB").convert("L"), cutoff=1)
    # E-ink washes out mid tones: a slight gamma and local contrast keep the
    # artwork's shading readable after dithering to four grays.
    gray = gray.point(lambda v: round(255 * (v / 255) ** 1.15))
    gray = gray.filter(ImageFilter.UnsharpMask(radius=1.5, percent=60, threshold=2))
    # A one-pixel dark rim separates light Pokémon from light walls: the
    # solid pixels that disappear when the silhouette is eroded by one pixel.
    solid = alpha.point(lambda a: 255 if a >= ALPHA_THRESHOLD else 0)
    inner = solid.filter(ImageFilter.MinFilter(3))
    rim = Image.composite(solid, Image.new("L", solid.size, 0), Image.eval(inner, lambda v: 255 - v))
    gray.paste(0, mask=rim)

    gray_px = gray.load()
    solid_px = solid.load()
    rows = []
    for y in range(size):
        rows.append([TRANSPARENT_INDEX if solid_px[x, y] == 0 else
                     min(GRAY_LEVELS - 1, round(gray_px[x, y] * (GRAY_LEVELS - 1) / 255)) for x in range(size)])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encode_bmp4(rows, size, size))


def encode_bmp4(rows: list[list[int]], width: int, height: int) -> bytes:
    """Bottom-up, uncompressed 4-bit BMP (Pillow only writes 1/8/24/32-bit)."""
    stride = ((width * 4 + 31) // 32) * 4
    pixel_bytes = bytearray()
    for row in reversed(rows):
        packed = bytearray(stride)
        for x, index in enumerate(row):
            packed[x // 2] |= index << (4 if x % 2 == 0 else 0)
        pixel_bytes += packed
    colours = palette()
    table = b"".join(bytes((colours[i * 3 + 2], colours[i * 3 + 1], colours[i * 3], 0)) for i in range(16))
    offset = 14 + 40 + len(table)
    header = struct.pack("<2sIHHI", b"BM", offset + len(pixel_bytes), 0, 0, offset)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 4, 0, len(pixel_bytes), 2835, 2835, 16, 16)
    return header + info + table + bytes(pixel_bytes)


def build(source: Path, output: Path, species_count: int = 151) -> None:
    for species_id in range(1, species_count + 1):
        image = source / f"{species_id}.png"
        if not image.is_file():
            raise ValueError(f"missing official artwork {image}")
        convert(image, output / "sleep" / f"{species_id:03}.bmp")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="folder with PokeAPI sprites/pokemon/other/official-artwork/<id>.png")
    parser.add_argument("--output", type=Path, required=True, help="art-pack root (the folder holding pokemon/'s contents)")
    parser.add_argument("--count", type=int, default=151)
    args = parser.parse_args()
    build(args.source, args.output, args.count)


if __name__ == "__main__":
    main()
