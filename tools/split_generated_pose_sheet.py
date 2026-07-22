#!/usr/bin/env python3
"""Split a verified 3×2 generated avatar sheet into six complete pose inputs."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    source = Image.open(args.input).convert("RGB")
    if source.width < 300 or source.height < 200:
        parser.error("input must be a full 3x2 pose sheet")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    # The generator delivers six equal grid cells.  Preserve each entire cell:
    # `package_generated_avb.py` later isolates the silhouette with its white
    # matte crop, so this split never guesses at limbs near a panel boundary.
    for row in range(2):
        top = row * source.height // 2
        bottom = (row + 1) * source.height // 2
        for column in range(3):
            left = column * source.width // 3
            right = (column + 1) * source.width // 3
            index = row * 3 + column
            destination = args.output_dir / f"pose-{index:02}.png"
            source.crop((left, top, right, bottom)).save(destination)
            print(destination)


if __name__ == "__main__":
    main()
