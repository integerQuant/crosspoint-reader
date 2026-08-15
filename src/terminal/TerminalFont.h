#pragma once

#include <cstdint>

class GfxRenderer;

namespace TerminalFont {

constexpr int GLYPH_WIDTH = 8;
constexpr int GLYPH_HEIGHT = 8;
constexpr int SCALE = 2;
constexpr int CELL_WIDTH = GLYPH_WIDTH * SCALE;
constexpr int CELL_HEIGHT = 20;

void drawCell(const GfxRenderer& renderer, int x, int y, uint8_t character, uint8_t attributes, bool cursor);

}  // namespace TerminalFont
