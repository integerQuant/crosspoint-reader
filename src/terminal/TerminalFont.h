#pragma once

#include <cstdint>

class GfxRenderer;

namespace TerminalFont {

constexpr int GLYPH_WIDTH = 8;
constexpr int GLYPH_HEIGHT = 8;
constexpr int SCALE_X = 1;
constexpr int SCALE_Y = 2;
constexpr int CELL_WIDTH = 9;
constexpr int CELL_HEIGHT = 18;

void drawCell(const GfxRenderer& renderer, int x, int y, uint8_t character, uint8_t attributes, bool cursor);

}  // namespace TerminalFont
