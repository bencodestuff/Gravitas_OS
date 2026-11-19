#!/usr/bin/env python3
"""Write a small, deterministic bitmap for the image root.

The wallpapers under files/Pictures are personal and not committed, so a fresh
clone has no picture at all. The image viewer opens the first picture in
/Pictures and prints which one it picked; with an empty directory there is
nothing to open. build_iso.sh calls this only when files/Pictures holds no
bitmap, so a checkout that has wallpapers ships exactly those and nothing else.

The output is 24-bit uncompressed, bottom-up, and depends only on the constants
below, so the same build always produces the same bytes.
"""

import struct
import sys

W, H = 96, 64

BASE = (0x1E, 0x1E, 0x2E)  # Catppuccin Mocha base
SURFACE = (0x31, 0x32, 0x44)  # ... and surface0
MAUVE = (0xCB, 0xA6, 0xF7)  # ... and mauve


def pixel(x, y):
    """A mauve diagonal band over a vertical base-to-surface gradient."""
    if abs(x * H - y * W) < 4 * W:
        return MAUVE
    t = y / (H - 1)
    return tuple(int(round(b + (s - b) * t)) for b, s in zip(BASE, SURFACE))


def write_bmp(path):
    row_bytes = W * 3
    stride = (row_bytes + 3) & ~3
    pixels = bytearray()
    for y in range(H - 1, -1, -1):  # bottom-up: the last row comes first
        for x in range(W):
            r, g, b = pixel(x, y)
            pixels += bytes((b, g, r))  # BMP stores blue first
        pixels += b"\0" * (stride - row_bytes)

    offset = 14 + 40
    header = b"BM" + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset)
    info = struct.pack(
        "<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(pixels), 2835, 2835, 0, 0
    )
    with open(path, "wb") as f:
        f.write(header + info + pixels)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mkpicture.py OUT.bmp")
    write_bmp(sys.argv[1])
