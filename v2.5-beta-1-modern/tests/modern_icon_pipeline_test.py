#!/usr/bin/env python3
"""Focused tests for the deterministic Reinked icon build and runtime contract."""

from __future__ import annotations

import importlib.util
import json
import re
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build-modern-icons.py"
SPEC = importlib.util.spec_from_file_location("build_modern_icons", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import {SCRIPT}")
icons = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = icons
SPEC.loader.exec_module(icons)


DETAILED_SVG = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <circle cx="128" cy="128" r="104" fill="#f6c445" stroke="#17191c" stroke-width="14"/>
  <path d="M52 112 Q128 38 204 112" fill="#4f91cf" stroke="#17191c" stroke-width="9"/>
  <circle cx="92" cy="124" r="15" fill="#f7f4e8" stroke="#17191c" stroke-width="7"/>
  <circle cx="164" cy="124" r="15" fill="#f7f4e8" stroke="#17191c" stroke-width="7"/>
  <circle cx="96" cy="126" r="6" fill="#17191c"/>
  <circle cx="160" cy="126" r="6" fill="#17191c"/>
  <path d="M92 174 Q128 202 166 168" fill="none" stroke="#17191c" stroke-width="11"/>
  <path d="M183 58 L213 35" fill="none" stroke="#dd6565" stroke-width="12"/>
</svg>
"""

class ModernIconPipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = icons.load_catalog()

    def test_manifest_pins_every_runtime_family_and_native_size(self) -> None:
        self.assertEqual(set(icons.EXPECTED_ICONS), {entry["name"] for entry in self.catalog.icons})
        self.assertEqual(set(icons.EXPECTED_STRIPS), {entry["name"] for entry in self.catalog.strips})
        self.assertEqual(set(icons.EXPECTED_EXPRESSIONS), {entry["name"] for entry in self.catalog.expressions})
        self.assertEqual(icons.ICO_SIZES, (16, 20, 24, 32, 48, 64, 128, 256))
        self.assertEqual(icons.STRIP_SIZES, (16, 20, 24, 32, 40, 48))
        outputs = set(icons.required_outputs(self.catalog))
        self.assertIn(Path("windows/chat.ico"), outputs)
        self.assertIn(Path("png/chat/256.png"), outputs)
        self.assertIn(Path("png/strips/balloons/20.png"), outputs)
        self.assertIn(Path("windows/expressions/laugh-80x104.bmp"), outputs)
        self.assertEqual(len(outputs), 345)
        self.assertIn(self.catalog.raw["art_review"]["status"], {"blocked", "approved"})

    def test_reserved_windows_blocks_match_all_six_sizes(self) -> None:
        resource_header = (ROOT / "v2.5-beta-1-modern" / "resource.h").read_text(encoding="cp1252")
        for strip, base in icons.EXPECTED_STRIP_RESOURCE_IDS.items():
            family = icons.STRIP_RESOURCE_FAMILIES[strip]
            for offset, size in enumerate(icons.STRIP_SIZES):
                self.assertRegex(resource_header, rf"(?m)^#define\s+IDR_MODERN_PNG_{family}_{size}\s+{base + offset}$")

    def test_generated_resource_includes_cover_every_png_without_wildcards(self) -> None:
        with tempfile.TemporaryDirectory(prefix="comic-chat-icon-test-") as temporary:
            root = Path(temporary)
            icons.build_resource_include(self.catalog, root)
            icons.build_resource_make_include(self.catalog, root)
            rc_lines = [line for line in (root / "windows/modern-icon-assets.rcinc").read_text().splitlines()
                        if " RCDATA " in line]
            self.assertEqual(len(rc_lines), 122)
            self.assertEqual(len(rc_lines), 11 * 6 + 8 * 7)
            self.assertTrue(any("IDR_MODERN_PNG_TOOLBAR_16" in line for line in rc_lines))
            self.assertTrue(any("IDR_MODERN_PNG_EXPR_LAUGH_80X104" in line for line in rc_lines))
            rc_text = "\n".join(rc_lines)
            self.assertIn(r'"..\\portable\\assets\\icons\\generated\\png\\strips\\toolbar\\16.png"',
                          rc_text)
            self.assertNotIn(r'"..\portable\assets\icons\generated', rc_text)
            make_lines = [line for line in (root / "windows/modern-icon-assets.makinc").read_text().splitlines()[2:]
                          if line.strip()]
            self.assertEqual(len(make_lines), 1 + 11 + 11 * 6 + 8 * 7)

    def test_portable_runtime_uses_metadata_and_high_dpi_png_alternates(self) -> None:
        app = (ROOT / "portable" / "src" / "app.cpp").read_text(encoding="utf-8")
        self.assertIn('app_id = "io.github.devinkbrown.ComicChatReinked"', app)
        self.assertIn("SDL_SetAppMetadata(", app)
        self.assertIn("SDL_LoadPNG(", app)
        self.assertIn("SDL_AddSurfaceAlternateImage(", app)
        self.assertIn("constexpr std::array modern_icon_sizes{32, 64, 128, 256}", app)
        self.assertNotIn("load_source_icon(", app)

    def test_linux_identity_files_match_sdl_identity(self) -> None:
        app_id = self.catalog.raw["app_id"]
        desktop = ROOT / "portable" / "packaging" / "linux" / f"{app_id}.desktop"
        metainfo = ROOT / "portable" / "packaging" / "linux" / f"{app_id}.metainfo.xml"
        desktop_text = desktop.read_text(encoding="utf-8")
        self.assertIn(f"Icon={app_id}\n", desktop_text)
        self.assertIn(f"StartupWMClass={app_id}\n", desktop_text)
        tree = ET.parse(metainfo)
        self.assertEqual(tree.getroot().findtext("id"), app_id)
        self.assertEqual(tree.getroot().find("launchable").text, f"{app_id}.desktop")

    def test_meson_native_asset_ladders_match_manifest(self) -> None:
        meson = (ROOT / "portable" / "assets" / "icons" / "meson.build").read_text(encoding="utf-8")

        def values(name: str) -> list[str]:
            match = re.search(rf"(?ms)^{re.escape(name)}\s*=\s*\[(.*?)\]", meson)
            self.assertIsNotNone(match, f"missing Meson array {name}")
            return re.findall(r"'([^']+)'|\b(\d+)\b", match.group(1))

        icon_sizes = [int(quoted or number) for quoted, number in values("modern_icon_sizes")]
        strip_sizes = [int(quoted or number) for quoted, number in values("modern_strip_sizes")]
        strip_names = [quoted or number for quoted, number in values("modern_strip_families")]
        expression_sizes = [quoted or number for quoted, number in values("modern_expression_sizes")]
        expression_names = [quoted or number for quoted, number in values("modern_expression_names")]
        self.assertEqual(icon_sizes, self.catalog.raw["ico_sizes"])
        self.assertEqual(strip_sizes, self.catalog.raw["strip_sizes"])
        self.assertEqual(set(strip_names), {entry["name"] for entry in self.catalog.strips})
        self.assertEqual(expression_sizes,
                         [f"{width}x{height}" for width, height in self.catalog.raw["expression_sizes"]])
        self.assertEqual(expression_names, [entry["name"] for entry in self.catalog.expressions])

    def test_opaque_ico_requires_explicit_boolean_opt_in(self) -> None:
        with tempfile.TemporaryDirectory(prefix="comic-chat-icon-test-") as temporary:
            root = Path(temporary)
            # Preserve the generator's exact 32-bit DIB container, but make
            # every frame's alpha fully opaque to isolate the policy toggle.
            data = bytearray((self.catalog.stage_root / "windows" / "chat.ico").read_bytes())
            frame_count = struct.unpack_from("<H", data, 4)[0]
            for index in range(frame_count):
                entry_offset = 6 + index * 16
                width_byte, _, _, _, _, _, length, payload_offset = struct.unpack_from(
                    "<BBBBHHII", data, entry_offset)
                size = 256 if width_byte == 0 else width_byte
                header_size = struct.unpack_from("<I", data, payload_offset)[0]
                self.assertLessEqual(header_size + size * size * 4, length)
                for pixel in range(size * size):
                    data[payload_offset + header_size + pixel * 4 + 3] = 255
            ico = root / "opaque.ico"
            ico.write_bytes(data)
            icons.validate_ico(ico, self.catalog, allow_opaque_canvas=True)
            with self.assertRaises(icons.IconBuildError):
                icons.validate_ico(ico, self.catalog, allow_opaque_canvas=False)

            invalid = json.loads(json.dumps(self.catalog.raw))
            invalid["icons"][0]["allow_opaque_canvas"] = "false"
            invalid_manifest = root / "manifest.json"
            invalid_manifest.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaises(icons.IconBuildError):
                icons.load_catalog(invalid_manifest)

    def test_vector_and_native_container_quality_smoke(self) -> None:
        with tempfile.TemporaryDirectory(prefix="comic-chat-icon-test-") as temporary:
            root = Path(temporary)
            source = root / "fixture.svg"
            source.write_text(DETAILED_SVG, encoding="utf-8")
            icons.audit_svg(source, min_drawables=6, min_paints=3)

            frames = []
            for size in icons.ICO_SIZES:
                frame = root / f"fixture-{size}.png"
                icons.render_svg(source, size, frame)
                frames.append(frame)
            ico = root / "fixture.ico"
            icons.run((icons.tool("icotool"), "--create", "--output", str(ico),
                       *(str(frame) for frame in frames)))
            icons.validate_ico(ico, self.catalog)

            strip_png = root / "strip.png"
            icons.run((icons.tool("magick"), str(frames[0]), str(frames[0]), "+append",
                       "-alpha", "on", "-strip", "-define", "png:color-type=6", str(strip_png)))
            icons.validate_png(strip_png, 32, 16)
            strip_bmp = root / "strip.bmp"
            icons.run((icons.tool("magick"), str(strip_png), "-alpha", "on", "-strip",
                       "-define", "bmp:format=bmp4", "-compress", "none", str(strip_bmp)))
            pixels = icons.decode_bmp4(strip_bmp, 32, 16)
            first_cell = [pixels[y * 32 + x] for y in range(16) for x in range(16)]
            icons.rgba_quality(first_cell, self.catalog.quality, "fixture strip", require_color=True)

    def test_simple_silhouette_is_rejected(self) -> None:
        simple = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><circle cx="16" cy="16" r="12" fill="#111"/></svg>'
        with tempfile.TemporaryDirectory(prefix="comic-chat-icon-test-") as temporary:
            path = Path(temporary) / "simple.svg"
            path.write_text(simple, encoding="utf-8")
            with self.assertRaises(icons.IconBuildError):
                icons.audit_svg(path, min_drawables=6, min_paints=3)


if __name__ == "__main__":
    unittest.main()
