#pragma once

#include <cstdint>

class GfxRenderer;

namespace TerminalFont {

constexpr int GLYPH_WIDTH = 8;
constexpr int GLYPH_HEIGHT = 16;
constexpr int CELL_WIDTH = GLYPH_WIDTH;
constexpr int CELL_HEIGHT = GLYPH_HEIGHT;

bool hasGlyph(uint16_t codepoint);
void drawGlyph(const GfxRenderer& renderer, int x, int y, int cellWidth, uint16_t glyphIndex, uint8_t attributes,
               bool cursor);

}  // namespace TerminalFont
