#!/usr/bin/env python3
"""Generate a compact knietty 8x16 lookup table from a BDF font."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    width: int
    height: int
    x_offset: int
    y_offset: int
    bitmap: tuple[int, ...]


@dataclass(frozen=True)
class SelectedGlyph:
    glyph: Glyph
    font_baseline: int
    downsample_wide: bool = False


def parse_bdf(path: Path) -> tuple[list[Glyph], int]:
    glyphs: list[Glyph] = []
    font_baseline = 12
    codepoint: int | None = None
    bounds: tuple[int, int, int, int] | None = None
    bitmap: list[int] | None = None

    for raw_line in path.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        if line.startswith("FONTBOUNDINGBOX "):
            _, height, _, y_offset = map(int, line.split()[1:])
            font_baseline = height + y_offset
        elif line.startswith("ENCODING "):
            codepoint = int(line.split()[1])
        elif line.startswith("BBX "):
            width, height, x_offset, y_offset = map(int, line.split()[1:])
            bounds = width, height, x_offset, y_offset
        elif line == "BITMAP":
            bitmap = []
        elif line == "ENDCHAR":
            if codepoint is not None and bounds is not None and bitmap is not None and 0 <= codepoint <= 0xFFFF:
                glyphs.append(Glyph(codepoint, *bounds, tuple(bitmap)))
            codepoint = None
            bounds = None
            bitmap = None
        elif bitmap is not None:
            bitmap.append(int(line, 16))

    return glyphs, font_baseline


def rasterize(glyph: Glyph, font_baseline: int, downsample_wide: bool = False) -> tuple[int, ...]:
    rows = [0] * 16
    for source_y, source_bits in enumerate(glyph.bitmap[: glyph.height]):
        target_y = font_baseline - glyph.y_offset - glyph.height + source_y
        if not 0 <= target_y < 16:
            continue
        for source_x in range(glyph.width):
            source_mask = 1 << (glyph.width - source_x - 1)
            target_x = (
                source_x * 8 // glyph.width
                if downsample_wide and glyph.width > 8
                else glyph.x_offset + source_x
            )
            if source_bits & source_mask and 0 <= target_x < 8:
                rows[target_y] |= 1 << (7 - target_x)
    return tuple(rows)


def codepoints_from_header(path: Path) -> set[int]:
    import re

    return {int(value, 16) for value in re.findall(r"\{0x([0-9a-fA-F]{4}), \{", path.read_text())}


def codepoints_from_manifest(path: Path) -> set[int]:
    codepoints: set[int] = set()
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        token = line.split()[0]
        if token.upper().startswith("U+"):
            token = token[2:]
        try:
            codepoint = int(token, 16)
        except ValueError as error:
            raise ValueError(f"{path}:{line_number}: invalid codepoint {token!r}") from error
        if not 0 <= codepoint <= 0xFFFF:
            raise ValueError(f"{path}:{line_number}: codepoint must fit the BMP")
        codepoints.add(codepoint)
    return codepoints


def write_header(
    glyphs: list[SelectedGlyph],
    output: Path,
    source_name: str,
    license_note: str,
    max_glyphs: int | None,
    expected_glyph_count: int | None,
) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        f"// Generated from {source_name} by scripts/generate_terminal_font.py.",
        f"// {license_note}",
        "namespace TerminalFontData {",
        "struct Glyph {",
        "  uint16_t codepoint;",
        "  uint8_t rows[16];",
        "};",
        "",
        "static constexpr Glyph GLYPHS[] = {",
    ]
    for selected in sorted(glyphs, key=lambda item: item.glyph.codepoint):
        glyph = selected.glyph
        rows = ", ".join(
            f"0x{row:02x}"
            for row in rasterize(glyph, selected.font_baseline, selected.downsample_wide)
        )
        lines.append(f"    {{0x{glyph.codepoint:04x}, {{{rows}}}}},")
    lines.extend(
        [
            "};",
            "static constexpr uint16_t GLYPH_COUNT = sizeof(GLYPHS) / sizeof(GLYPHS[0]);",
            "static_assert(sizeof(Glyph) == 18);",
        ]
    )
    if expected_glyph_count is not None:
        lines.append(f"static_assert(GLYPH_COUNT == {expected_glyph_count});")
    if max_glyphs is not None:
        lines.append(f"static_assert(GLYPH_COUNT <= {max_glyphs});")
    lines.extend(["}  // namespace TerminalFontData", ""])
    output.write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("bdf", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("src/terminal/TerminalFontData.generated.h"),
    )
    parser.add_argument("--source-name", default="8x16 BDF font")
    parser.add_argument("--license-note", default="See the font license files distributed with knietty.")
    parser.add_argument(
        "--subset-from",
        type=Path,
        help="include only codepoints present in an existing generated header",
    )
    parser.add_argument(
        "--exclude-wide",
        action="store_true",
        help="drop glyphs whose BDF bounding box is wider than one 8-pixel cell",
    )
    parser.add_argument(
        "--add-codepoints-from",
        type=Path,
        help="add the BMP codepoints listed in a manifest after applying --subset-from",
    )
    parser.add_argument(
        "--fallback-bdf",
        type=Path,
        help="use this BDF when an explicitly added codepoint is absent from the primary BDF",
    )
    parser.add_argument(
        "--fallback-source-name",
        help="append fallback provenance to the generated source comment",
    )
    parser.add_argument(
        "--downsample-wide-fallback",
        action="store_true",
        help="explicitly scale 16-pixel fallback glyphs into one 8-pixel cell",
    )
    parser.add_argument("--max-glyphs", type=int, help="fail if the generated table exceeds this many glyphs")
    parser.add_argument(
        "--expected-glyph-count",
        type=int,
        help="fail unless the generated table contains exactly this many glyphs",
    )
    args = parser.parse_args()
    primary_glyphs, font_baseline = parse_bdf(args.bdf)
    glyphs = primary_glyphs
    if args.subset_from:
        subset = codepoints_from_header(args.subset_from)
        glyphs = [glyph for glyph in glyphs if glyph.codepoint in subset]
    # Unifont is duospaced. Its 16-pixel glyphs require a future wcwidth-aware
    # screen model and must not be silently cropped. Existing Spleen generation
    # intentionally preserves its few overhanging bounding boxes, so filtering
    # is opt-in rather than a global behavior change.
    if args.exclude_wide:
        glyphs = [glyph for glyph in glyphs if glyph.width <= 8]
    selected = [SelectedGlyph(glyph, font_baseline) for glyph in glyphs]

    if args.add_codepoints_from:
        requested = codepoints_from_manifest(args.add_codepoints_from)
        selected_codepoints = {item.glyph.codepoint for item in selected}
        primary_by_codepoint = {glyph.codepoint: glyph for glyph in primary_glyphs}
        fallback_by_codepoint: dict[int, Glyph] = {}
        fallback_baseline = 0
        if args.fallback_bdf:
            fallback_glyphs, fallback_baseline = parse_bdf(args.fallback_bdf)
            fallback_by_codepoint = {glyph.codepoint: glyph for glyph in fallback_glyphs}

        for codepoint in sorted(requested - selected_codepoints):
            primary = primary_by_codepoint.get(codepoint)
            if primary is not None and primary.width <= 8:
                selected.append(SelectedGlyph(primary, font_baseline))
                continue
            fallback = fallback_by_codepoint.get(codepoint)
            if fallback is None:
                parser.error(f"U+{codepoint:04X} is unavailable in the primary and fallback BDFs")
            if fallback.width > 8 and not args.downsample_wide_fallback:
                parser.error(
                    f"U+{codepoint:04X} is {fallback.width} pixels wide; "
                    "pass --downsample-wide-fallback to adapt it explicitly"
                )
            selected.append(SelectedGlyph(fallback, fallback_baseline, fallback.width > 8))

    if args.max_glyphs is not None and len(selected) > args.max_glyphs:
        parser.error(f"generated {len(selected)} glyphs, exceeding --max-glyphs {args.max_glyphs}")
    if args.expected_glyph_count is not None and len(selected) != args.expected_glyph_count:
        parser.error(
            f"generated {len(selected)} glyphs, expected exactly {args.expected_glyph_count}"
        )

    source_name = args.source_name
    if args.fallback_source_name and args.add_codepoints_from:
        source_name += f" with selected glyphs from {args.fallback_source_name}"
    write_header(
        selected,
        args.output,
        source_name,
        args.license_note,
        args.max_glyphs,
        args.expected_glyph_count,
    )


if __name__ == "__main__":
    main()
