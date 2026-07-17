#!/usr/bin/env python3
"""Build and audit the Comic Chat: Reinked native icon catalog.

The SVG masters are authored by hand.  This tool only performs deterministic
rasterization, native container assembly, provenance recording, and mechanical
quality gates.  It deliberately never traces or synthesizes artwork.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path, PurePath
from typing import Any, Iterable, Sequence


REPOSITORY = Path(__file__).resolve().parents[1]
ASSET_ROOT = REPOSITORY / "portable" / "assets" / "icons"
DEFAULT_MANIFEST = ASSET_ROOT / "manifest.json"

ICO_SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
STRIP_SIZES = (16, 20, 24, 32, 40, 48)
OPTICAL_SIZES = (16, 20, 24, 32)
EXPRESSION_SIZES = ((20, 26), (25, 33), (30, 39), (40, 52), (50, 65), (60, 78), (80, 104))
EXPRESSION_OPTICAL_SIZES = ((20, 26),)
REQUIRED_ICON_OPTICAL_SIZES = (16, 32)
REQUIRED_INTERMEDIATE_ICON_OPTICAL = ("chat", "avatar")
EXPECTED_ICONS = {
    "chat": "IDR_MAINFRAME",
    "chatdoc": "IDI_CHAT_DOC",
    "room": "IDI_CHAT_ROOM",
    "ruleset": "IDI_RULESET",
    "avatar": "IDI_AVATAR",
    "backgd": "IDI_BACKGROUND",
    "ratings": "IDI_RATINGS",
    "whisper": "IDI_WHISPER",
    "notif": "IDI_NOTIF",
    "tosrv": "IDI_CONNECT_SRV",
    "tonet": "IDI_CONNECT_NET",
}
EXPECTED_STRIPS = {
    "toolbar": (
        "IDR_MAINFRAME",
        ("connect", "disconnect", "enter-room", "leave-room", "create-room",
         "comic-view", "text-view", "room-list", "user-list", "favorites"),
    ),
    "texttool": (
        "IDR_TEXTTOOLBAR",
        ("font", "color", "bold", "italic", "underline", "fixed-pitch", "symbol"),
    ),
    "usertool": (
        "IDR_USERTOOLBAR",
        ("away", "identity", "ignore", "whisper", "email", "homepage", "netmeeting"),
    ),
    "tabbar": ("IDB_TABS", ("room", "new-content", "status", "alert")),
    "member": ("IDB_MEMBER", ("normal", "host", "spectator", "ignored", "away")),
    "balloons": (
        "IDB_SAY_BAR",
        ("say", "think", "whisper", "action", "whisper-action", "sound", "whisper-sound"),
    ),
    "oldnew": ("IDB_OLDNEW", ("new-indicator",)),
    "connect": ("IDB_CONNECT", ("connected", "disconnected")),
    "stopped": ("IDB_STOPPED", ("stopped",)),
    "inactive": ("IDB_INACTIVE", ("inactive",)),
    "active": ("IDB_ACTIVE", ("active",)),
}
EXPECTED_STRIP_RESOURCE_IDS = {
    "toolbar": 4000,
    "tabbar": 4010,
    "balloons": 4020,
    "member": 4030,
    "texttool": 4040,
    "usertool": 4050,
    "connect": 4060,
    "oldnew": 4070,
    "inactive": 4080,
    "active": 4090,
    "stopped": 4100,
}
STRIP_RESOURCE_FAMILIES = {
    "toolbar": "TOOLBAR",
    "tabbar": "TABS",
    "balloons": "SAY",
    "member": "MEMBER",
    "texttool": "TEXT",
    "usertool": "USER",
    "connect": "CONNECT",
    "oldnew": "OLDNEW",
    "inactive": "INACTIVE",
    "active": "ACTIVE",
    "stopped": "STOPPED",
}
EXPECTED_EXPRESSIONS = {
    "happy": ("IDR_HAPPY", "fc_hap_l.bmp"),
    "coy": ("IDR_COY", "fc_coy_l.bmp"),
    "bored": ("IDR_BORED", "fc_bor_l.bmp"),
    "scared": ("IDR_SCARED", "fc_sca_l.bmp"),
    "sad": ("IDR_SAD", "fc_sad_l.bmp"),
    "angry": ("IDR_ANGRY", "fc_ang_l.bmp"),
    "shout": ("IDR_SHOUT", "fc_sho_l.bmp"),
    "laugh": ("IDR_LAUGH", "fc_laf_l.bmp"),
}
DRAWABLE_ELEMENTS = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon"}
FORBIDDEN_ELEMENTS = {"script", "image", "foreignObject", "iframe", "audio", "video", "text"}
PAINT_ATTRIBUTES = {"fill", "stroke", "stop-color", "flood-color", "lighting-color", "color"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_SVG_BYTES = 2 * 1024 * 1024


class IconBuildError(RuntimeError):
    """A user-actionable catalog, toolchain, or asset validation failure."""


@dataclass(frozen=True)
class PixelQuality:
    min_visible_fraction: float
    max_visible_fraction: float
    min_chromatic_fraction: float
    min_unique_visible_colors: int
    min_alpha_levels: int


@dataclass(frozen=True)
class Catalog:
    path: Path
    raw: dict[str, Any]
    stage_root: Path
    reference_root: Path
    icons: tuple[dict[str, Any], ...]
    strips: tuple[dict[str, Any], ...]
    expressions: tuple[dict[str, str], ...]
    quality: PixelQuality


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IconBuildError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_sha256(path: Path) -> str:
    """Hash SVG text independently of checkout newline conversion."""
    data = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")


def repo_path(raw: Any, field: str) -> Path:
    require(isinstance(raw, str) and raw, f"{field} must be a non-empty repository-relative path")
    candidate = Path(raw)
    require(not candidate.is_absolute() and ".." not in candidate.parts, f"{field} escapes the repository")
    result = (REPOSITORY / candidate).resolve()
    require(result.is_relative_to(REPOSITORY.resolve()), f"{field} escapes the repository")
    return result


def load_catalog(path: Path = DEFAULT_MANIFEST) -> Catalog:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise IconBuildError(f"cannot read {path}: {error}") from error
    require(isinstance(raw, dict) and raw.get("schema") == 1, "manifest schema must be 1")
    require(tuple(raw.get("ico_sizes", ())) == ICO_SIZES,
            f"ICO ladder must be exactly {ICO_SIZES}")
    require(tuple(raw.get("strip_sizes", ())) == STRIP_SIZES,
            f"strip ladder must be exactly {STRIP_SIZES}")
    require(tuple(raw.get("required_optical_sizes", ())) == OPTICAL_SIZES,
            f"compact optical ladder must be exactly {OPTICAL_SIZES}")
    require(tuple(raw.get("required_icon_optical_sizes", ())) == REQUIRED_ICON_OPTICAL_SIZES,
            f"global standalone optical redraws must be exactly {REQUIRED_ICON_OPTICAL_SIZES}")
    require(tuple(raw.get("required_intermediate_icon_optical", ())) ==
            REQUIRED_INTERMEDIATE_ICON_OPTICAL,
            "20/24px authored overrides must be limited to the shared cameo-heavy chat/avatar family")
    require(tuple(tuple(size) for size in raw.get("expression_sizes", ())) == EXPRESSION_SIZES,
            f"expression ladder must be exactly {EXPRESSION_SIZES}")
    require(tuple(tuple(size) for size in raw.get("required_expression_optical_sizes", ())) ==
            EXPRESSION_OPTICAL_SIZES,
            f"compact expression ladder must be exactly {EXPRESSION_OPTICAL_SIZES}")
    require(raw.get("app_id") == "io.github.devinkbrown.ComicChatReinked",
            "app_id must match the desktop/Wayland reverse-domain identity")
    require(raw.get("expression_resource_base") == 4200,
            "expression RCDATA block must begin at the reserved resource ID 4200")
    art_review = raw.get("art_review")
    require(isinstance(art_review, dict) and isinstance(art_review.get("revision"), str) and
            art_review.get("status") in {"blocked", "approved"} and isinstance(art_review.get("reason"), str),
            "art_review must name a revision, status, and review reason")

    icons = raw.get("icons")
    require(isinstance(icons, list), "icons must be a list")
    for entry in icons:
        require(isinstance(entry, dict), "each standalone icon must be an object")
        if "allow_opaque_canvas" in entry:
            require(type(entry["allow_opaque_canvas"]) is bool,
                    f"{entry.get('name')!r} allow_opaque_canvas must be a JSON boolean")
    actual_icons = {entry.get("name"): entry.get("resource") for entry in icons if isinstance(entry, dict)}
    require(actual_icons == EXPECTED_ICONS,
            "standalone icon coverage must exactly match the 11 legacy semantic resources")
    require(len(icons) == len(EXPECTED_ICONS), "standalone icons contain duplicate entries")

    strips = raw.get("strips")
    require(isinstance(strips, list), "strips must be a list")
    actual_strips: dict[str, tuple[str, tuple[str, ...]]] = {}
    for entry in strips:
        require(isinstance(entry, dict), "each strip must be an object")
        name, resource, cells = entry.get("name"), entry.get("resource"), entry.get("cells")
        require(isinstance(cells, list) and all(isinstance(item, str) for item in cells),
                f"{name!r} strip cells must be strings")
        require(isinstance(entry.get("mask"), str) and re.fullmatch(r"#[0-9a-fA-F]{6}", entry["mask"]),
                f"{name!r} strip mask must be #rrggbb")
        require(name not in actual_strips, f"duplicate strip {name!r}")
        require(entry.get("generated_resource_base") == EXPECTED_STRIP_RESOURCE_IDS.get(name),
                f"{name!r} generated RCDATA block drifted from the reserved Windows runtime contract")
        actual_strips[name] = (resource, tuple(cells))
    require(actual_strips == EXPECTED_STRIPS,
            "native strip coverage/order drifted from the legacy resource and command map")

    expressions = raw.get("expressions")
    require(isinstance(expressions, list), "expressions must be a list")
    actual_expressions = {
        entry.get("name"): (entry.get("resource"), entry.get("reference"))
        for entry in expressions if isinstance(entry, dict)
    }
    require(actual_expressions == EXPECTED_EXPRESSIONS,
            "bodycam expression coverage must exactly match all eight runtime resources")
    require(len(expressions) == len(EXPECTED_EXPRESSIONS), "bodycam expressions contain duplicate entries")
    for offset, entry in enumerate(expressions):
        require(entry.get("generated_resource_offset") == offset,
                "bodycam expression RCDATA offsets must retain canonical emotion order")

    quality = raw.get("quality")
    require(isinstance(quality, dict), "quality must be an object")
    pixel_quality = PixelQuality(
        float(quality.get("min_visible_fraction", -1)),
        float(quality.get("max_visible_fraction", -1)),
        float(quality.get("min_chromatic_fraction", -1)),
        int(quality.get("min_unique_visible_colors", -1)),
        int(quality.get("min_alpha_levels", -1)),
    )
    require(0 < pixel_quality.min_visible_fraction < pixel_quality.max_visible_fraction < 1,
            "visible-fraction limits are invalid")
    require(0 <= pixel_quality.min_chromatic_fraction < 1, "chromatic-fraction limit is invalid")
    require(pixel_quality.min_unique_visible_colors >= 4, "unique-color limit is too weak")
    require(pixel_quality.min_alpha_levels >= 2, "alpha-level limit is too weak")
    for key in ("icon_min_drawables", "icon_min_paints", "strip_min_drawables", "strip_min_paints"):
        require(isinstance(quality.get(key), int) and quality[key] >= 2, f"{key} is too weak")
    for entry in strips:
        exceptions = entry.get("quality_exceptions", [])
        require(isinstance(exceptions, list),
                f"{entry['name']!r} quality_exceptions must be a list")
        seen_exceptions: set[tuple[str, int]] = set()
        for exception in exceptions:
            require(isinstance(exception, dict),
                    f"{entry['name']!r} quality exception must be an object")
            allowed = {"cell", "size", "reason", "min_alpha_levels", "max_visible_fraction"}
            require(set(exception) <= allowed,
                    f"{entry['name']!r} quality exception has unsupported fields")
            cell, size = exception.get("cell"), exception.get("size")
            require(cell in entry["cells"] and size in STRIP_SIZES,
                    f"{entry['name']!r} quality exception targets an unknown cell or size")
            key = (cell, size)
            require(key not in seen_exceptions,
                    f"duplicate quality exception for {entry['name']}/{cell}@{size}")
            seen_exceptions.add(key)
            reason = exception.get("reason")
            require(isinstance(reason, str) and len(reason.strip()) >= 24,
                    f"{entry['name']}/{cell}@{size} quality exception needs a specific reason")
            has_alpha = "min_alpha_levels" in exception
            has_coverage = "max_visible_fraction" in exception
            require(has_alpha or has_coverage,
                    f"{entry['name']}/{cell}@{size} quality exception changes no threshold")
            if has_alpha:
                alpha = exception["min_alpha_levels"]
                require(type(alpha) is int and 2 <= alpha < pixel_quality.min_alpha_levels,
                        f"{entry['name']}/{cell}@{size} alpha exception is not narrowly relaxed")
            if has_coverage:
                coverage = exception["max_visible_fraction"]
                require(isinstance(coverage, (int, float)) and not isinstance(coverage, bool) and
                        pixel_quality.max_visible_fraction < float(coverage) < 1,
                        f"{entry['name']}/{cell}@{size} coverage exception is not narrowly relaxed")

    return Catalog(
        path=path,
        raw=raw,
        stage_root=repo_path(raw.get("stage_root"), "stage_root"),
        reference_root=repo_path(raw.get("reference_root"), "reference_root"),
        icons=tuple(icons),
        strips=tuple(strips),
        expressions=tuple(expressions),
        quality=pixel_quality,
    )


def local_name(name: str) -> str:
    return name.rsplit("}", 1)[-1]


def style_paints(value: str) -> Iterable[str]:
    for declaration in value.split(";"):
        key, separator, paint = declaration.partition(":")
        if separator and key.strip() in PAINT_ATTRIBUTES:
            yield paint.strip()


def audit_svg(path: Path, *, min_drawables: int, min_paints: int,
              aspect: tuple[int, int] = (1, 1)) -> None:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise IconBuildError(f"cannot read SVG {path}: {error}") from error
    require(0 < len(data) <= MAX_SVG_BYTES, f"{path} is empty or exceeds {MAX_SVG_BYTES} bytes")
    lower = data.lower()
    require(b"<!doctype" not in lower and b"<!entity" not in lower,
            f"{path} contains a DTD/entity declaration")
    require(b"@import" not in lower, f"{path} imports an external stylesheet")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise IconBuildError(f"invalid SVG {path}: {error}") from error
    require(local_name(root.tag) == "svg", f"{path} root element is not svg")
    view_box = root.attrib.get("viewBox", "").replace(",", " ").split()
    require(len(view_box) == 4, f"{path} must define a four-number viewBox")
    try:
        left, top, width, height = (float(item) for item in view_box)
    except ValueError as error:
        raise IconBuildError(f"{path} has an invalid viewBox") from error
    expected_ratio = aspect[0] / aspect[1]
    require(width > 0 and height > 0 and abs(width / height - expected_ratio) < 0.001 and
            abs(left) < 0.001 and abs(top) < 0.001,
            f"{path} viewBox must be a positive {aspect[0]}:{aspect[1]} canvas rooted at 0,0")

    drawables = 0
    paints: set[str] = set()
    for element in root.iter():
        tag = local_name(element.tag)
        require(tag not in FORBIDDEN_ELEMENTS, f"{path} contains forbidden <{tag}> content")
        if tag in DRAWABLE_ELEMENTS:
            drawables += 1
        for raw_name, value in element.attrib.items():
            name = local_name(raw_name)
            require(not name.lower().startswith("on"), f"{path} contains event handler {name}")
            if name == "href":
                require(value.startswith("#"), f"{path} contains an external href")
            if "url(" in value.lower():
                for target in re.findall(r"url\(([^)]+)\)", value, re.IGNORECASE):
                    require(target.strip(" \"'").startswith("#"), f"{path} references an external URL")
            if name in PAINT_ATTRIBUTES and value not in {"", "none", "inherit"}:
                paints.add(value.strip())
            if name == "style":
                paints.update(paint for paint in style_paints(value) if paint not in {"", "none", "inherit"})
    require(drawables >= min_drawables,
            f"{path} has {drawables} drawable elements; detailed artwork requires at least {min_drawables}")
    require(len(paints) >= min_paints,
            f"{path} has {len(paints)} paint treatments; detailed artwork requires at least {min_paints}")


def icon_master(name: str) -> Path:
    return ASSET_ROOT / "masters" / f"{name}.svg"


def icon_optical(name: str, size: int) -> Path:
    return ASSET_ROOT / "optical" / str(size) / f"{name}.svg"


def strip_master(strip: str, cell: str) -> Path:
    return ASSET_ROOT / "masters" / "strips" / strip / f"{cell}.svg"


def strip_optical(strip: str, cell: str, size: int) -> Path:
    return ASSET_ROOT / "optical" / str(size) / "strips" / strip / f"{cell}.svg"


def expression_master(name: str) -> Path:
    return ASSET_ROOT / "masters" / "expressions" / f"{name}.svg"


def expression_optical(name: str, width: int, height: int) -> Path:
    return ASSET_ROOT / "optical" / f"{width}x{height}" / "expressions" / f"{name}.svg"


def expected_sources(catalog: Catalog) -> tuple[Path, ...]:
    paths: list[Path] = []
    for entry in catalog.icons:
        name = entry["name"]
        paths.append(icon_master(name))
        paths.extend(icon_optical(name, size) for size in REQUIRED_ICON_OPTICAL_SIZES)
        if name in REQUIRED_INTERMEDIATE_ICON_OPTICAL:
            paths.extend(icon_optical(name, size) for size in (20, 24))
    for entry in catalog.strips:
        for cell in entry["cells"]:
            paths.append(strip_master(entry["name"], cell))
    for entry in catalog.expressions:
        paths.append(expression_master(entry["name"]))
        paths.extend(expression_optical(entry["name"], width, height)
                     for width, height in EXPRESSION_OPTICAL_SIZES)
    return tuple(paths)


def selected_sources(catalog: Catalog) -> tuple[Path, ...]:
    """Return every source that affects a generated output, including optional overrides."""
    paths = set(expected_sources(catalog))
    for entry in catalog.icons:
        paths.update(source_for_icon(entry["name"], size) for size in ICO_SIZES)
    for entry in catalog.strips:
        for cell in entry["cells"]:
            paths.update(source_for_strip(entry["name"], cell, size) for size in STRIP_SIZES)
    for entry in catalog.expressions:
        paths.update(source_for_expression(entry["name"], width, height)
                     for width, height in EXPRESSION_SIZES)
    return tuple(sorted(paths))


def lint_sources(catalog: Catalog, *, complete: bool) -> tuple[Path, ...]:
    required = expected_sources(catalog)
    missing = [path for path in required if not path.is_file()]
    if complete and missing:
        preview = "\n".join(f"  {path.relative_to(REPOSITORY)}" for path in missing[:20])
        suffix = f"\n  ... and {len(missing) - 20} more" if len(missing) > 20 else ""
        raise IconBuildError(f"modern icon source catalog is incomplete ({len(missing)} missing):\n{preview}{suffix}")

    quality = catalog.raw["quality"]
    candidates = set(required)
    for entry in catalog.icons:
        candidates.update(icon_optical(entry["name"], size) for size in OPTICAL_SIZES)
    for entry in catalog.strips:
        for cell in entry["cells"]:
            candidates.update(strip_optical(entry["name"], cell, size) for size in OPTICAL_SIZES)
    for entry in catalog.expressions:
        candidates.update(expression_optical(entry["name"], width, height)
                          for width, height in EXPRESSION_SIZES)
    for path in sorted(candidates):
        if not path.is_file():
            continue
        is_strip = "strips" in path.parts
        is_expression = "expressions" in path.parts
        audit_svg(
            path,
            min_drawables=quality["strip_min_drawables"] if is_strip else quality["icon_min_drawables"],
            min_paints=quality["strip_min_paints"] if is_strip else quality["icon_min_paints"],
            aspect=(20, 26) if is_expression else (1, 1),
        )
    return tuple(missing)


def tool(name: str) -> str:
    # ImageMagick 7 installs the `magick` front end, while stable Linux and BSD
    # distributions may still package ImageMagick 6 as `convert`.  The command
    # surface used by this pipeline is shared by both versions.
    candidates = ("magick", "convert") if name == "magick" else (name,)
    for candidate in candidates:
        result = shutil.which(candidate)
        if result is not None:
            return result
    raise IconBuildError(f"required icon build tool is missing: {name}")


def canonical_relative(path: PurePath) -> str:
    """Return the stable slash-separated spelling used by catalog.lock.json."""
    return path.as_posix()


def run(command: Sequence[str]) -> None:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C", "SOURCE_DATE_EPOCH": "0"})
    try:
        completed = subprocess.run(command, env=environment, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, check=False, timeout=60)
    except subprocess.TimeoutExpired as error:
        raise IconBuildError(f"command timed out ({' '.join(command)})") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        raise IconBuildError(f"command failed ({' '.join(command)}): {detail}")


def render_svg(source: Path, width: int, destination: Path, height: int | None = None) -> None:
    height = height if height is not None else width
    destination.parent.mkdir(parents=True, exist_ok=True)
    run((tool("rsvg-convert"), "--dpi-x", "96", "--dpi-y", "96", "--width", str(width),
         "--height", str(height), "--format", "png", "--output", str(destination), str(source)))
    os.chmod(destination, 0o644)


def rgba_quality(pixels: Sequence[tuple[int, int, int, int]], quality: PixelQuality,
                 label: str, *, require_color: bool, require_transparency: bool = True) -> None:
    require(pixels, f"{label} has no pixels")
    visible = [pixel for pixel in pixels if pixel[3] > 8]
    visible_fraction = len(visible) / len(pixels)
    require(visible_fraction >= quality.min_visible_fraction and
            (not require_transparency or visible_fraction <= quality.max_visible_fraction),
            f"{label} visible coverage {visible_fraction:.3f} is outside the detailed-icon range")
    if require_transparency:
        require(any(pixel[3] <= 8 for pixel in pixels), f"{label} has no transparent background")
    alpha_levels = {pixel[3] for pixel in pixels}
    require(not require_transparency or len(alpha_levels) >= quality.min_alpha_levels,
            f"{label} has only {len(alpha_levels)} alpha levels; antialiased detail is missing")
    visible_colors = {(r, g, b, a) for r, g, b, a in visible}
    require(len(visible_colors) >= quality.min_unique_visible_colors,
            f"{label} has only {len(visible_colors)} visible colors; artwork is too simple")
    if require_color:
        chromatic = sum(1 for r, g, b, _ in visible if max(r, g, b) - min(r, g, b) >= 12)
        fraction = chromatic / len(visible)
        require(fraction >= quality.min_chromatic_fraction,
                f"{label} has insufficient chromatic structure ({fraction:.3f})")


def decode_dib_payload(payload: bytes, size: int, label: str) -> list[tuple[int, int, int, int]]:
    require(not payload.startswith(PNG_SIGNATURE), f"{label} is PNG-compressed; 32-bit DIB is required")
    require(len(payload) >= 40, f"{label} DIB header is truncated")
    header_size, width, combined_height = struct.unpack_from("<Iii", payload, 0)
    planes, depth = struct.unpack_from("<HH", payload, 12)
    compression = struct.unpack_from("<I", payload, 16)[0]
    require(header_size >= 40 and header_size <= len(payload), f"{label} DIB header size is invalid")
    require((width, abs(combined_height)) == (size, size * 2), f"{label} DIB dimensions are invalid")
    require(planes == 1 and depth == 32 and compression == 0,
            f"{label} must be an uncompressed 32-bit Windows DIB")
    pixel_bytes = size * size * 4
    require(header_size + pixel_bytes <= len(payload), f"{label} DIB pixels are truncated")
    result: list[tuple[int, int, int, int]] = []
    for offset in range(header_size, header_size + pixel_bytes, 4):
        blue, green, red, alpha = payload[offset:offset + 4]
        result.append((red, green, blue, alpha))
    return result


def validate_ico(path: Path, catalog: Catalog, *, allow_opaque_canvas: bool = False) -> None:
    data = path.read_bytes()
    require(len(data) >= 6, f"{path} is truncated")
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    require((reserved, kind, count) == (0, 1, len(ICO_SIZES)),
            f"{path} must contain exactly {len(ICO_SIZES)} icon frames")
    require(len(data) >= 6 + count * 16, f"{path} directory is truncated")
    actual: list[int] = []
    ranges: list[tuple[int, int]] = []
    for index in range(count):
        entry = 6 + index * 16
        width_byte, height_byte, colors, entry_reserved, planes, depth, length, offset = struct.unpack_from(
            "<BBBBHHII", data, entry)
        width = 256 if width_byte == 0 else width_byte
        height = 256 if height_byte == 0 else height_byte
        require(width == height, f"{path} frame {index + 1} is not square")
        require(colors == 0 and entry_reserved == 0 and planes == 1 and depth == 32,
                f"{path} frame {width}px directory metadata is not true-color 32-bit")
        require(offset >= 6 + count * 16 and length > 0 and offset + length <= len(data),
                f"{path} frame {width}px payload range is invalid")
        ranges.append((offset, offset + length))
        pixels = decode_dib_payload(data[offset:offset + length], width, f"{path}:{width}px")
        rgba_quality(pixels, catalog.quality, f"{path}:{width}px", require_color=True,
                     require_transparency=not allow_opaque_canvas)
        actual.append(width)
    require(tuple(actual) == ICO_SIZES, f"{path} frame order/sizes are {actual}, expected {ICO_SIZES}")
    for previous, current in zip(sorted(ranges), sorted(ranges)[1:]):
        require(previous[1] <= current[0], f"{path} has overlapping frame payloads")


def decode_bmp4(path: Path, expected_width: int, expected_height: int) -> list[tuple[int, int, int, int]]:
    data = path.read_bytes()
    require(data[:2] == b"BM" and len(data) >= 70, f"{path} is not a Windows bitmap")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, depth = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    require(header_size >= 108, f"{path} must use an alpha-capable BMPv4/v5 header")
    require((width, abs(signed_height)) == (expected_width, expected_height),
            f"{path} dimensions are {width}x{abs(signed_height)}, expected {expected_width}x{expected_height}")
    require(planes == 1 and depth == 32 and compression == 3,
            f"{path} must be a 32-bit BI_BITFIELDS bitmap")
    require(pixel_offset + expected_width * expected_height * 4 <= len(data), f"{path} pixels are truncated")
    pixels: list[tuple[int, int, int, int]] = []
    for y in range(expected_height):
        stored_y = expected_height - 1 - y if signed_height > 0 else y
        row = pixel_offset + stored_y * expected_width * 4
        for x in range(expected_width):
            blue, green, red, alpha = data[row + x * 4:row + x * 4 + 4]
            pixels.append((red, green, blue, alpha))
    return pixels


def validate_png(path: Path, expected_width: int, expected_height: int) -> None:
    data = path.read_bytes()
    require(len(data) >= 33 and data.startswith(PNG_SIGNATURE), f"{path} is not a PNG")
    ihdr_length = struct.unpack_from(">I", data, 8)[0]
    require(ihdr_length == 13 and data[12:16] == b"IHDR", f"{path} has an invalid IHDR")
    width, height, depth, color_type = struct.unpack_from(">IIBB", data, 16)
    require((width, height) == (expected_width, expected_height),
            f"{path} dimensions are {width}x{height}, expected {expected_width}x{expected_height}")
    require(depth == 8 and color_type in {4, 6}, f"{path} must retain an 8-bit alpha channel")
    require(b"IEND" in data[-16:], f"{path} is truncated before IEND")


def required_outputs(catalog: Catalog) -> tuple[Path, ...]:
    result: list[Path] = [
        Path("windows") / "modern-icon-assets.rcinc",
        Path("windows") / "modern-icon-assets.makinc",
    ]
    for entry in catalog.icons:
        name = entry["name"]
        result.append(Path("windows") / f"{name}.ico")
        result.extend(Path("png") / name / f"{size}.png" for size in ICO_SIZES)
    for entry in catalog.strips:
        for size in STRIP_SIZES:
            result.append(Path("windows") / "strips" / f"{entry['name']}-{size}.bmp")
            result.append(Path("png") / "strips" / entry["name"] / f"{size}.png")
    for entry in catalog.expressions:
        name = entry["name"]
        for width, height in EXPRESSION_SIZES:
            result.append(Path("windows") / "expressions" / f"{name}-{width}x{height}.bmp")
            result.append(Path("png") / "expressions" / name / f"{width}x{height}.png")
    return tuple(result)


def source_for_icon(name: str, size: int) -> Path:
    override = icon_optical(name, size)
    if override.is_file():
        return override
    # 20/24px are compact UI frames. Families without a dedicated redraw are
    # intentionally rendered from their authored 32px optical drawing, never
    # from the presentation-size master.
    if size in (20, 24):
        return icon_optical(name, 32)
    return icon_master(name)


def source_for_strip(strip: str, cell: str, size: int) -> Path:
    override = strip_optical(strip, cell, size)
    return override if override.is_file() else strip_master(strip, cell)


def source_for_expression(name: str, width: int, height: int) -> Path:
    override = expression_optical(name, width, height)
    return override if override.is_file() else expression_master(name)


def quality_for_strip(catalog: Catalog, entry: dict[str, Any], cell: str, size: int) -> PixelQuality:
    """Apply only a documented exception for one exact native strip cell and size."""
    quality = catalog.quality
    for exception in entry.get("quality_exceptions", []):
        if exception["cell"] == cell and exception["size"] == size:
            return PixelQuality(
                quality.min_visible_fraction,
                float(exception.get("max_visible_fraction", quality.max_visible_fraction)),
                quality.min_chromatic_fraction,
                quality.min_unique_visible_colors,
                int(exception.get("min_alpha_levels", quality.min_alpha_levels)),
            )
    return quality


def build_icon(catalog: Catalog, entry: dict[str, Any], output: Path, work: Path) -> None:
    name = entry["name"]
    frames: list[Path] = []
    for size in ICO_SIZES:
        frame = work / f"{size}.png"
        render_svg(source_for_icon(name, size), size, frame)
        frames.append(frame)
        png_output = output / "png" / name / f"{size}.png"
        png_output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(frame, png_output)
        os.chmod(png_output, 0o644)
    ico_output = output / "windows" / f"{name}.ico"
    ico_output.parent.mkdir(parents=True, exist_ok=True)
    # Do not pass --raw/-r: every frame must remain an uncompressed DIB for the
    # portable parser and for deterministic Windows resource inspection.
    run((tool("icotool"), "--create", "--output", str(ico_output), *(str(frame) for frame in frames)))
    os.chmod(ico_output, 0o644)
    validate_ico(ico_output, catalog, allow_opaque_canvas=bool(entry.get("allow_opaque_canvas")))


def build_strip(catalog: Catalog, entry: dict[str, Any], output: Path, work: Path) -> None:
    name = entry["name"]
    for size in STRIP_SIZES:
        frames: list[Path] = []
        for index, cell in enumerate(entry["cells"]):
            frame = work / str(size) / f"{index:02d}-{cell}.png"
            render_svg(source_for_strip(name, cell, size), size, frame)
            frames.append(frame)
        png_destination = output / "png" / "strips" / name / f"{size}.png"
        png_destination.parent.mkdir(parents=True, exist_ok=True)
        run((tool("magick"), *(str(frame) for frame in frames), "+append", "-alpha", "on", "-strip",
             "-define", "png:color-type=6", str(png_destination)))
        os.chmod(png_destination, 0o644)
        destination = output / "windows" / "strips" / f"{name}-{size}.bmp"
        destination.parent.mkdir(parents=True, exist_ok=True)
        run((tool("magick"), str(png_destination), "-alpha", "on", "-strip",
             "-define", "bmp:format=bmp4", "-compress", "none", str(destination)))
        os.chmod(destination, 0o644)
        pixels = decode_bmp4(destination, size * len(frames), size)
        for index, cell in enumerate(entry["cells"]):
            cell_pixels = [pixels[y * size * len(frames) + index * size + x]
                           for y in range(size) for x in range(size)]
            rgba_quality(cell_pixels, quality_for_strip(catalog, entry, cell, size),
                         f"{destination}:{cell}", require_color=False)


def build_expression(catalog: Catalog, entry: dict[str, str], output: Path, work: Path) -> None:
    name = entry["name"]
    for width, height in EXPRESSION_SIZES:
        frame = work / f"{width}x{height}.png"
        render_svg(source_for_expression(name, width, height), width, frame, height)
        png_output = output / "png" / "expressions" / name / f"{width}x{height}.png"
        png_output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(frame, png_output)
        os.chmod(png_output, 0o644)
        bmp_output = output / "windows" / "expressions" / f"{name}-{width}x{height}.bmp"
        bmp_output.parent.mkdir(parents=True, exist_ok=True)
        run((tool("magick"), str(frame), "-alpha", "on", "-strip", "-define", "bmp:format=bmp4",
             "-compress", "none", str(bmp_output)))
        os.chmod(bmp_output, 0o644)
        pixels = decode_bmp4(bmp_output, width, height)
        rgba_quality(pixels, catalog.quality, f"{bmp_output}", require_color=True)


def build_resource_include(catalog: Catalog, output: Path) -> None:
    lines = [
        "// Generated by scripts/build-modern-icons.py. Do not edit.",
        "// Alpha-preserving, equal-cell PNG strips selected by native DPI.",
        "",
    ]
    for entry in catalog.strips:
        family = STRIP_RESOURCE_FAMILIES[entry["name"]]
        base = entry["generated_resource_base"]
        for offset, size in enumerate(STRIP_SIZES):
            symbol = f"IDR_MODERN_PNG_{family}_{size}"
            require(base + offset == EXPECTED_STRIP_RESOURCE_IDS[entry["name"]] + offset,
                    f"resource block arithmetic drifted for {symbol}")
            resource_path = str(
                Path("..") / "portable" / "assets" / "icons" / "generated" /
                "png" / "strips" / entry["name"] / f"{size}.png"
            ).replace("/", "\\").replace("\\", "\\\\")
            lines.append(f'{symbol:<34} RCDATA  "{resource_path}"')
        lines.append("")
    for size_index, (width, height) in enumerate(EXPRESSION_SIZES):
        for entry in catalog.expressions:
            symbol = f"IDR_MODERN_PNG_EXPR_{entry['name'].upper()}_{width}X{height}"
            resource_path = str(
                Path("..") / "portable" / "assets" / "icons" / "generated" /
                "png" / "expressions" / entry["name"] / f"{width}x{height}.png"
            ).replace("/", "\\").replace("\\", "\\\\")
            lines.append(f'{symbol:<44} RCDATA  "{resource_path}"')
        lines.append("")
    destination = output / "windows" / "modern-icon-assets.rcinc"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(lines), encoding="ascii", newline="\n")
    os.chmod(destination, 0o644)


def build_resource_make_include(catalog: Catalog, output: Path) -> None:
    prefix = Path("..") / "portable" / "assets" / "icons" / "generated"
    inputs: list[Path] = [prefix / "windows" / "modern-icon-assets.rcinc"]
    inputs.extend(prefix / "windows" / f"{entry['name']}.ico" for entry in catalog.icons)
    for entry in catalog.strips:
        inputs.extend(prefix / "png" / "strips" / entry["name"] / f"{size}.png"
                      for size in STRIP_SIZES)
    for entry in catalog.expressions:
        inputs.extend(prefix / "png" / "expressions" / entry["name"] / f"{width}x{height}.png"
                      for width, height in EXPRESSION_SIZES)
    lines = ["# Generated by scripts/build-modern-icons.py. Do not edit.", "MODERN_ICON_RESOURCE_INPUTS= \\"]
    for index, path in enumerate(inputs):
        suffix = " \\" if index + 1 < len(inputs) else ""
        lines.append(f"\t{str(path).replace('/', chr(92))}{suffix}")
    destination = output / "windows" / "modern-icon-assets.makinc"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    os.chmod(destination, 0o644)


def validate_resource_contract(catalog: Catalog, root: Path) -> None:
    include = (root / "windows" / "modern-icon-assets.rcinc").read_text(encoding="ascii")
    header = (REPOSITORY / "v2.5-beta-1-modern" / "resource.h").read_text(encoding="cp1252")
    declarations = re.findall(r"^(IDR_MODERN_PNG_[A-Z0-9_]+)\s+RCDATA\s+\"([^\"]+)\"$",
                              include, re.MULTILINE)
    expected_declaration_count = (len(catalog.strips) * len(STRIP_SIZES) +
                                  len(catalog.expressions) * len(EXPRESSION_SIZES))
    require(expected_declaration_count == 122,
            "Windows runtime contract must contain 66 strip and 56 expression declarations")
    require(len(declarations) == expected_declaration_count,
            "generated RCDATA include has missing or duplicate icon declarations")
    actual_symbols = {symbol for symbol, _ in declarations}
    expected_symbols: set[str] = set()
    for entry in catalog.strips:
        family = STRIP_RESOURCE_FAMILIES[entry["name"]]
        for offset, size in enumerate(STRIP_SIZES):
            symbol = f"IDR_MODERN_PNG_{family}_{size}"
            expected_symbols.add(symbol)
            expected_value = entry["generated_resource_base"] + offset
            require(re.search(rf"^#define\s+{re.escape(symbol)}\s+{expected_value}$", header, re.MULTILINE) is not None,
                    f"resource.h is missing the reserved mapping {symbol}={expected_value}")
    expression_base = catalog.raw["expression_resource_base"]
    for size_index, (width, height) in enumerate(EXPRESSION_SIZES):
        for entry in catalog.expressions:
            symbol = f"IDR_MODERN_PNG_EXPR_{entry['name'].upper()}_{width}X{height}"
            expected_symbols.add(symbol)
            expected_value = expression_base + size_index * 10 + entry["generated_resource_offset"]
            require(re.search(rf"^#define\s+{re.escape(symbol)}\s+{expected_value}$", header, re.MULTILINE) is not None,
                    f"resource.h is missing the reserved mapping {symbol}={expected_value}")
    require(actual_symbols == expected_symbols, "generated RCDATA symbols drifted from the runtime contract")
    make_include = (root / "windows" / "modern-icon-assets.makinc").read_text(encoding="ascii")
    make_inputs = [line.strip().removesuffix(" \\") for line in make_include.splitlines()[2:] if line.strip()]
    expected_input_count = (1 + len(catalog.icons) + len(catalog.strips) * len(STRIP_SIZES) +
                            len(catalog.expressions) * len(EXPRESSION_SIZES))
    require(len(make_inputs) == expected_input_count and len(set(make_inputs)) == expected_input_count,
            "generated NMAKE dependency include has missing or duplicate resource inputs")
    require(all(path.startswith("..\\portable\\assets\\icons\\generated\\") for path in make_inputs),
            "generated NMAKE dependency escaped the canonical modern asset tree")


def tool_version(name: str) -> str:
    completed = subprocess.run((tool(name), "--version"), text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    return completed.stdout.splitlines()[0].strip()


def source_fingerprint(catalog: Catalog) -> tuple[str, dict[str, str]]:
    sources = {
        canonical_relative(path.relative_to(REPOSITORY)): source_sha256(path)
        for path in selected_sources(catalog)
    }
    payload = {"manifest": catalog.raw, "sources": sources}
    return hashlib.sha256(canonical_json(payload)).hexdigest(), sources


def write_lock(catalog: Catalog, output: Path) -> None:
    fingerprint, sources = source_fingerprint(catalog)
    outputs = {
        canonical_relative(path): sha256(output / path)
        for path in required_outputs(catalog)
    }
    lock = {
        "schema": 1,
        "source_fingerprint": fingerprint,
        "sources": sources,
        "outputs": outputs,
        "toolchain": {
            "rsvg-convert": tool_version("rsvg-convert"),
            "icotool": tool_version("icotool"),
            "magick": tool_version("magick"),
        },
    }
    (output / "catalog.lock.json").write_bytes(canonical_json(lock))
    os.chmod(output / "catalog.lock.json", 0o644)


def build_catalog(catalog: Catalog, output: Path) -> None:
    require(catalog.raw["art_review"]["status"] == "approved",
            "generation is blocked until the source-reconstructed face revision has explicit visual approval")
    lint_sources(catalog, complete=True)
    output.mkdir(parents=True, exist_ok=True)
    # Keep raster work beside the output as well. This honors the caller's
    # chosen filesystem and avoids small or separately-quota'd system temp
    # volumes on Windows and BSD builders.
    with tempfile.TemporaryDirectory(prefix=".comic-chat-icons-work-",
                                     dir=output.parent) as temporary:
        work = Path(temporary)
        for entry in catalog.icons:
            build_icon(catalog, entry, output, work / "icons" / entry["name"])
        for entry in catalog.strips:
            build_strip(catalog, entry, output, work / "strips" / entry["name"])
        for entry in catalog.expressions:
            build_expression(catalog, entry, output, work / "expressions" / entry["name"])
        build_resource_include(catalog, output)
        build_resource_make_include(catalog, output)
    write_lock(catalog, output)


def verify_catalog(catalog: Catalog, root: Path | None = None) -> None:
    root = root or catalog.stage_root
    lock_path = root / "catalog.lock.json"
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise IconBuildError(f"cannot read generated catalog lock {lock_path}: {error}") from error
    require(lock.get("schema") == 1, "generated catalog lock schema is invalid")
    required = required_outputs(catalog)
    expected_keys = {canonical_relative(path) for path in required}
    require(set(lock.get("outputs", {})) == expected_keys, "generated output coverage is incomplete or stale")
    fingerprint, sources = source_fingerprint(catalog)
    require(lock.get("source_fingerprint") == fingerprint and lock.get("sources") == sources,
            "generated outputs are stale relative to the SVG masters or manifest")
    for relative in required:
        path = root / relative
        require(path.is_file(), f"missing generated output {path}")
        require(sha256(path) == lock["outputs"][canonical_relative(relative)],
                f"generated output hash drifted: {path}")
    actual_files = {
        path.relative_to(root) for path in root.rglob("*") if path.is_file()
    }
    require(actual_files == set(required) | {Path("catalog.lock.json")},
            "generated tree contains stale or untracked outputs")
    validate_resource_contract(catalog, root)

    for entry in catalog.icons:
        generated = root / "windows" / f"{entry['name']}.ico"
        validate_ico(generated, catalog, allow_opaque_canvas=bool(entry.get("allow_opaque_canvas")))
        reference = catalog.reference_root / f"{entry['name']}.ico"
        require(reference.is_file(), f"missing Microsoft reference {reference}")
        require(sha256(generated) != sha256(reference), f"{generated} is still the original Microsoft ICO")
        for size in ICO_SIZES:
            validate_png(root / "png" / entry["name"] / f"{size}.png", size, size)
    for entry in catalog.strips:
        for size in STRIP_SIZES:
            generated = root / "windows" / "strips" / f"{entry['name']}-{size}.bmp"
            validate_png(root / "png" / "strips" / entry["name"] / f"{size}.png",
                         size * len(entry["cells"]), size)
            pixels = decode_bmp4(generated, size * len(entry["cells"]), size)
            for index, cell in enumerate(entry["cells"]):
                cell_pixels = [pixels[y * size * len(entry["cells"]) + index * size + x]
                               for y in range(size) for x in range(size)]
                rgba_quality(cell_pixels, quality_for_strip(catalog, entry, cell, size),
                             f"{generated}:{cell}", require_color=False)
        reference = catalog.reference_root / f"{entry['name']}.bmp"
        require(reference.is_file(), f"missing Microsoft reference {reference}")
        require(sha256(root / "windows" / "strips" / f"{entry['name']}-16.bmp") != sha256(reference),
                f"{entry['name']} 16px strip is still the Microsoft bitmap")
    for entry in catalog.expressions:
        for width, height in EXPRESSION_SIZES:
            generated = root / "windows" / "expressions" / f"{entry['name']}-{width}x{height}.bmp"
            validate_png(root / "png" / "expressions" / entry["name"] / f"{width}x{height}.png",
                         width, height)
            pixels = decode_bmp4(generated, width, height)
            rgba_quality(pixels, catalog.quality, f"{generated}", require_color=True)
        reference = catalog.reference_root / entry["reference"]
        require(reference.is_file(), f"missing Microsoft reference {reference}")
        require(sha256(root / "windows" / "expressions" / f"{entry['name']}-20x26.bmp") != sha256(reference),
                f"{entry['name']} 20x26 expression is still the Microsoft bitmap")


def replace_generated_tree(catalog: Catalog, source: Path) -> None:
    destination = catalog.stage_root
    destination.parent.mkdir(parents=True, exist_ok=True)
    backup = destination.with_name(destination.name + ".previous")
    if backup.exists():
        shutil.rmtree(backup)
    try:
        if destination.exists():
            destination.rename(backup)
        source.rename(destination)
    except BaseException:
        if not destination.exists() and backup.exists():
            backup.rename(destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def generate(catalog: Catalog) -> None:
    # Stage beside the destination so the final directory rename is atomic and
    # can never cross filesystems, while remaining portable to Windows and BSD.
    catalog.stage_root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".comic-chat-icons-output-",
                                     dir=catalog.stage_root.parent) as temporary:
        output = Path(temporary) / "generated"
        build_catalog(catalog, output)
        verify_catalog(catalog, output)
        # TemporaryDirectory must not remove the directory after it becomes the
        # checked-in catalog, so move it atomically into its canonical parent.
        replace_generated_tree(catalog, output)


def verify_rebuild(catalog: Catalog) -> None:
    verify_catalog(catalog)
    catalog.stage_root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".comic-chat-icons-rebuild-",
                                     dir=catalog.stage_root.parent) as temporary:
        rebuilt = Path(temporary) / "generated"
        build_catalog(catalog, rebuilt)
        verify_catalog(catalog, rebuilt)
        for relative in required_outputs(catalog):
            require(sha256(catalog.stage_root / relative) == sha256(rebuilt / relative),
                    f"non-deterministic rebuild: {relative}")


def command_line(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    commands = parser.add_subparsers(dest="command", required=True)
    lint = commands.add_parser("lint", help="validate manifest and available SVG sources")
    lint.add_argument("--complete", action="store_true", help="require every master and compact override")
    commands.add_parser("generate", help="rebuild the complete canonical runtime asset tree")
    verify = commands.add_parser("verify", help="audit the checked-in generated runtime asset tree")
    verify.add_argument("--rebuild", action="store_true", help="also prove a byte-identical clean rebuild")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = command_line(argv if argv is not None else sys.argv[1:])
        catalog = load_catalog(arguments.manifest)
        if arguments.command == "lint":
            missing = lint_sources(catalog, complete=arguments.complete)
            print("modern icon manifest passed: 11 ICO families, 11 native strips, "
                  f"8 bodycam expressions; {len(missing)} source files pending")
        elif arguments.command == "generate":
            generate(catalog)
            print(f"generated and verified {len(required_outputs(catalog))} modern icon artifacts")
        elif arguments.command == "verify":
            if arguments.rebuild:
                verify_rebuild(catalog)
            else:
                verify_catalog(catalog)
            print("modern icon catalog passed semantic, frame, alpha, detail, freshness, and provenance gates")
        return 0
    except IconBuildError as error:
        print(f"modern icon build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
