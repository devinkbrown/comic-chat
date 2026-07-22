#!/usr/bin/env python3
"""Package generated Comic Chat avatar pose art as a native simple-avatar AVB.

The file structure is a direct, intentionally small implementation of the
historical CAvatarSimple::Save path: AVATARHEADER, AK_ICON_NEW,
AK_NBODIES2, AK_STARTDATA, embedded DIBs, then AK_ENDDATA.  White pixels are
kept white because Comic Chat's CBodySingle uses SRCAND; on a comic panel they
leave the destination unchanged, which is the same practical matte used by
the original simple-avatar art.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from PIL import Image, ImageChops


MAGIC = 0x8181
SIMPLE_AVATAR = 1
VERSION = 2
AK_NAME = 1
AK_NBODIES2 = 12
AK_STARTDATA = 6
AK_ENDDATA = 7
AK_ICON_NEW = 256
AK_COPYRIGHT = 259
AIF_DIB = 0
AIP_NOPALETTE = 0

# The authored order is also the simple-avatar gesture ordinal used by
# CAvatarSimple when an old client asks for a UDI pose.
POSES = (
    ("neutral", 9),
    ("laugh", 8),
    ("surprised", 7),
    ("angry", 2),
    ("sad", 4),
    ("action", 10),
)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def variable_record(tag: int, payload: bytes) -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError(f"record {tag} is too large")
    return u16(tag) + u16(len(payload)) + payload


def normalize_pose(path: Path) -> Image.Image:
    """Crop a nearly-white generated card and return a compact white-matte pose."""
    source = Image.open(path).convert("RGB")
    # The generator's card background varies a few values around white.  Map
    # it to pure white so SRCAND does not leave a visible rectangle.
    pixels = source.load()
    for y in range(source.height):
        for x in range(source.width):
            red, green, blue = pixels[x, y]
            if red >= 245 and green >= 245 and blue >= 245:
                pixels[x, y] = (255, 255, 255)

    inverted = ImageChops.invert(source)
    bbox = inverted.getbbox()
    if bbox is None:
        raise ValueError(f"{path} contains no visible pose")
    left, top, right, bottom = bbox
    pad = 12
    crop = source.crop((max(0, left - pad), max(0, top - pad), min(source.width, right + pad), min(source.height, bottom + pad)))
    crop.thumbnail((210, 260), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (240, 280), "white")
    canvas.paste(crop, ((canvas.width - crop.width) // 2, canvas.height - crop.height - 6))
    return canvas


def bmp24(image: Image.Image) -> bytes:
    """Return a bottom-up BI_RGB BMP stream accepted by CAvatarDIB::Load."""
    image = image.convert("RGB")
    width, height = image.size
    stride = ((width * 3 + 3) // 4) * 4
    pixels = bytearray(stride * height)
    for y in range(height):
        dest = (height - 1 - y) * stride
        for x in range(width):
            red, green, blue = image.getpixel((x, y))
            offset = dest + x * 3
            pixels[offset : offset + 3] = bytes((blue, green, red))
    pixel_offset = 14 + 40
    size = pixel_offset + len(pixels)
    file_header = b"BM" + u32(size) + u16(0) + u16(0) + u32(pixel_offset)
    info_header = u32(40) + struct.pack("<iiHHIIiiII", width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    return file_header + info_header + pixels


def build(name: str, copyright_text: str, pose_paths: list[Path], portrait_icon: bool = False) -> bytes:
    if len(pose_paths) != len(POSES):
        raise ValueError(f"expected {len(POSES)} pose PNGs")
    pose_bmps = [bmp24(normalize_pose(path)) for path in pose_paths]
    icon_source = normalize_pose(pose_paths[0])
    if portrait_icon:
        # Gallery and roster icons need a readable face at 64px. Keep the
        # upper body while the pose records below retain the complete figure.
        # Crop to the actual visible silhouette rather than the source canvas:
        # narrow figures such as Xeno otherwise read as a tiny full body.
        silhouette = ImageChops.invert(icon_source)
        left, top, right, bottom = silhouette.getbbox() or (0, 0, icon_source.width, icon_source.height)
        portrait_bottom = min(bottom, top + max(64, (bottom - top) * 3 // 5))
        pad = 8
        icon_source = icon_source.crop((max(0, left - pad), max(0, top - pad), min(icon_source.width, right + pad), min(icon_source.height, portrait_bottom + pad)))
        icon_source.thumbnail((58, 58), Image.Resampling.LANCZOS)
        icon = Image.new("RGB", (64, 64), "white")
        icon.paste(icon_source, ((64 - icon_source.width) // 2, (64 - icon_source.height) // 2))
    else:
        icon = icon_source.resize((64, 64), Image.Resampling.LANCZOS)
    icon_bmp = bmp24(icon)

    name_record = u16(AK_NAME) + name.encode("utf-8") + b"\0"
    copyright_record = variable_record(AK_COPYRIGHT, copyright_text.encode("utf-8") + b"\0")
    icon_record_size = 2 + 2 + 6
    body_table_size = 2 + 2 + len(POSES) * 25
    data_start = 6 + len(name_record) + len(copyright_record) + icon_record_size + body_table_size + 2
    icon_offset = data_start
    offsets: list[int] = []
    cursor = icon_offset + len(icon_bmp)
    for bmp in pose_bmps:
        offsets.append(cursor)
        cursor += len(bmp)

    result = bytearray()
    result += u16(MAGIC) + u16(SIMPLE_AVATAR) + u16(VERSION)
    result += name_record
    result += copyright_record
    result += variable_record(AK_ICON_NEW, u32(icon_offset) + bytes((AIF_DIB, AIP_NOPALETTE)))
    result += u16(AK_NBODIES2) + u16(len(POSES))
    for offset, (_, emotion) in zip(offsets, POSES):
        # AVATARBODYDATA::newdata: three offsets, emotion, intensity, face
        # point, three AIF_DIB bytes, three AIP_NOPALETTE bytes.
        result += u32(offset) + u32(0) + u32(0)
        result += u16(emotion) + bytes((0,)) + struct.pack("<hh", 120, 60)
        result += bytes((AIF_DIB, AIF_DIB, AIF_DIB, AIP_NOPALETTE, AIP_NOPALETTE, AIP_NOPALETTE))
    result += u16(AK_STARTDATA)
    if len(result) != data_start:
        raise AssertionError(f"metadata size drift: {len(result)} != {data_start}")
    result += icon_bmp
    result += b"".join(pose_bmps)
    result += u16(AK_ENDDATA)
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--copyright", required=True, dest="copyright_text")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--portrait-icon", action="store_true", help="crop the icon to a readable head-and-shoulders portrait")
    parser.add_argument("poses", nargs=len(POSES), type=Path, metavar="POSE")
    args = parser.parse_args()
    for pose in args.poses:
        if not pose.is_file():
            parser.error(f"pose file not found: {pose}")
    data = build(args.name, args.copyright_text, args.poses, args.portrait_icon)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"wrote {args.output} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})")


if __name__ == "__main__":
    main()
