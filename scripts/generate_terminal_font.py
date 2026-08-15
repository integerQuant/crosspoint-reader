#!/usr/bin/env python3
"""Generate knietty's compact Spleen lookup table from an 8x16 BDF."""

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


def parse_bdf(path: Path) -> list[Glyph]:
    glyphs: list[Glyph] = []
    codepoint: int | None = None
    bounds: tuple[int, int, int, int] | None = None
    bitmap: list[int] | None = None

    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("ENCODING "):
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

    return glyphs


def rasterize(glyph: Glyph) -> tuple[int, ...]:
    rows = [0] * 16
    for source_y, source_bits in enumerate(glyph.bitmap[: glyph.height]):
        target_y = 12 - glyph.y_offset - glyph.height + source_y
        if not 0 <= target_y < 16:
            continue
        for source_x in range(glyph.width):
            source_mask = 1 << (glyph.width - source_x - 1)
            target_x = glyph.x_offset + source_x
            if source_bits & source_mask and 0 <= target_x < 8:
                rows[target_y] |= 1 << (7 - target_x)
    return tuple(rows)


def write_header(glyphs: list[Glyph], output: Path) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// Generated from Spleen 8x16 2.2.0 by scripts/generate_terminal_font.py.",
        "// Copyright (c) 2018-2026 Frederic Cambus; BSD-2-Clause.",
        "namespace TerminalFontData {",
        "struct Glyph {",
        "  uint16_t codepoint;",
        "  uint8_t rows[16];",
        "};",
        "",
        "static constexpr Glyph GLYPHS[] = {",
    ]
    for glyph in sorted(glyphs, key=lambda item: item.codepoint):
        rows = ", ".join(f"0x{row:02x}" for row in rasterize(glyph))
        lines.append(f"    {{0x{glyph.codepoint:04x}, {{{rows}}}}},")
    lines.extend(
        [
            "};",
            "static constexpr uint16_t GLYPH_COUNT = sizeof(GLYPHS) / sizeof(GLYPHS[0]);",
            "static_assert(sizeof(Glyph) == 18);",
            "}  // namespace TerminalFontData",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("bdf", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("src/terminal/TerminalFontData.generated.h"),
    )
    args = parser.parse_args()
    write_header(parse_bdf(args.bdf), args.output)


if __name__ == "__main__":
    main()
