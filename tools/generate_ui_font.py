#!/usr/bin/env python3
"""Generate the embedded neutral sans-serif atlas used by desktop chrome."""

from __future__ import annotations

import hashlib
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont


EXPECTED_SHA256 = "baccc64becc3eb7d104b7c84d99f5314a0a1f896e2b3ea6c2f22fc08d2003bee"
FIRST = 0x20
COUNT = 95
SIZE = 16


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_ui_font.py /path/to/LiberationSans-Regular.ttf")
    source = pathlib.Path(sys.argv[1])
    actual = hashlib.sha256(source.read_bytes()).hexdigest()
    if actual != EXPECTED_SHA256:
        raise SystemExit(f"unexpected font source hash: {actual}")

    font = ImageFont.truetype(source, SIZE)
    ascent, descent = font.getmetrics()
    coverage = bytearray()
    records: list[tuple[int, int, int, int, int, int]] = []
    for codepoint in range(FIRST, FIRST + COUNT):
        character = chr(codepoint)
        left, top, right, bottom = font.getbbox(character)
        width = max(0, right - left)
        height = max(0, bottom - top)
        offset = len(coverage)
        if width and height:
            image = Image.new("L", (width, height), 0)
            ImageDraw.Draw(image).text((-left, -top), character, font=font, fill=255)
            coverage.extend(image.tobytes())
        records.append((round(font.getlength(character)), width, height, left, top, offset))

    rows = "\n".join(
        f"    .{{ .advance = {advance}, .w = {width}, .h = {height}, "
        f".xoff = {xoff}, .yoff = {yoff}, .off = {offset} }},"
        for advance, width, height, xoff, yoff, offset in records
    )
    source_text = (
        "//! Neutral UI atlas generated from OFL-licensed Liberation Sans Regular.\n"
        "//! See `src/render/LIBERATION_SANS_LICENSE.txt`.\n\n"
        "pub const first: u8 = 0x20;\n"
        f"pub const count: usize = {COUNT};\n"
        f"pub const request_size: i32 = {SIZE};\n"
        f"pub const ascent: i32 = {ascent};\n"
        f"pub const descent: i32 = {descent};\n"
        f"pub const line_height: i32 = {font.font.height};\n\n"
        "pub const Glyph = struct { advance: u8, w: u8, h: u8, xoff: i8, yoff: i8, off: u32 };\n\n"
        f"pub const glyphs = [count]Glyph{{\n{rows}\n}};\n\n"
        'pub const coverage = @embedFile("fontdata_ui.bin");\n'
    )
    root = pathlib.Path(__file__).resolve().parents[1] / "src/render"
    (root / "fontdata_ui.bin").write_bytes(coverage)
    (root / "font_ui.zig").write_text(source_text, encoding="utf-8")


if __name__ == "__main__":
    main()
