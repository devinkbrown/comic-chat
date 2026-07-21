#!/usr/bin/env python3
"""Split a generated 3x2 Comic Chat pose sheet into deterministic AVB inputs."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sheet", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    image = Image.open(args.sheet).convert("RGB")
    # Models sometimes honor the requested six cells as 3x2 landscape and
    # sometimes as 2x3 portrait. Both retain the same reading-order poses.
    cols, rows = (3, 2) if image.width >= image.height else (2, 3)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for index in range(6):
        row, col = divmod(index, cols)
        left, right = col * image.width // cols, (col + 1) * image.width // cols
        top, bottom = row * image.height // rows, (row + 1) * image.height // rows
        cell = image.crop((left, top, right, bottom))
        cell.save(args.output_dir / f"pose-{index:02}.png")


if __name__ == "__main__":
    main()
