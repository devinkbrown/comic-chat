#!/usr/bin/env python3
"""Audit the original Microsoft say-strip layout and pixel topology."""

from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path


MODERN = Path(__file__).resolve().parents[1]
BALLOONS = MODERN / "res" / "balloons.bmp"
LOADER = MODERN / "modernicons.cpp"
OFFSETS = (0, 17, 34, 51, 68, 85, 102)
WIDTHS = (17, 17, 17, 17, 17, 17, 16)
EXPECTED_PIXELS = (155, 165, 153, 51, 35, 117, 111)
EXPECTED_COMPONENTS_8 = (1, 1, 1, 7, 14, 4, 7)
SOURCE_SHA256 = "686e6b0f610ef2c7ee4b59c158cc6d1b02acd26100cc9a72aa84c77a9da9e764"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_bmp4(path: Path) -> tuple[int, int, list[list[tuple[int, int, int]]]]:
    data = path.read_bytes()
    require(data[:2] == b"BM", "balloons.bmp is not a Windows bitmap")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    require(planes == 1 and bits == 4 and compression == 0,
            "say strip must remain the original uncompressed 4-bit BMP")
    height = abs(signed_height)
    palette_offset = 14 + dib_size
    palette = []
    for index in range(16):
        blue, green, red, _ = struct.unpack_from("<BBBB", data, palette_offset + index * 4)
        palette.append((red, green, blue))
    stride = ((width * bits + 31) // 32) * 4
    rows: list[list[tuple[int, int, int]]] = []
    for y in range(height):
        stored_y = height - 1 - y if signed_height > 0 else y
        row_offset = pixel_offset + stored_y * stride
        row = []
        for x in range(width):
            packed = data[row_offset + x // 2]
            palette_index = packed >> 4 if x % 2 == 0 else packed & 0x0F
            row.append(palette[palette_index])
        rows.append(row)
    return width, height, rows


def component_count_8(points: set[tuple[int, int]]) -> int:
    remaining = set(points)
    components = 0
    while remaining:
        components += 1
        pending = [remaining.pop()]
        while pending:
            x, y = pending.pop()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    neighbor = (x + dx, y + dy)
                    if neighbor in remaining:
                        remaining.remove(neighbor)
                        pending.append(neighbor)
    return components


def main() -> None:
    require(hashlib.sha256(BALLOONS.read_bytes()).hexdigest() == SOURCE_SHA256,
            "the authoritative Microsoft balloons.bmp changed")
    width, height, rows = read_bmp4(BALLOONS)
    require((width, height) == (118, 17), "say strip must remain exactly 118 x 17")
    require(OFFSETS[-1] + WIDTHS[-1] == width,
            "logical say cells do not cover the complete source strip")

    mask = (192, 192, 192)
    pixel_counts = []
    component_counts = []
    for left, cell_width in zip(OFFSETS, WIDTHS):
        ink = {
            (x, y)
            for y in range(height)
            for x in range(cell_width)
            if rows[y][left + x] != mask
        }
        pixel_counts.append(len(ink))
        component_counts.append(component_count_8(ink))
    require(tuple(pixel_counts) == EXPECTED_PIXELS,
            f"say-strip cell pixels changed: {pixel_counts}")
    require(tuple(component_counts) == EXPECTED_COMPONENTS_8,
            f"say-strip 8-connected topology changed: {component_counts}")

    loader = LOADER.read_text(encoding="cp1252")
    require(re.search(r"kSayBarOffsets\[\]\s*=\s*\{\s*0,\s*17,\s*34,\s*51,\s*68,\s*85,\s*102\s*\}", loader),
            "runtime loader lost the seven canonical source offsets")
    require(re.search(r"kSayBarWidths\[\]\s*=\s*\{\s*17,\s*17,\s*17,\s*17,\s*17,\s*17,\s*16\s*\}", loader),
            "runtime loader lost the final 16-pixel Microsoft cell")
    require("source_left, 0, source_width" in loader,
            "runtime no longer scales each physical source cell independently")
    print("source strip topology passed: 118x17, widths 17/17/17/17/17/17/16, "
          "all seven Microsoft cell masks preserved")


if __name__ == "__main__":
    main()
