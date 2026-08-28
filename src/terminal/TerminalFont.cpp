#include "TerminalFont.h"

#include <GfxRenderer.h>

#include "TerminalGlyphs.h"
#include "TerminalScreen.h"

bool TerminalFont::hasGlyph(const uint16_t codepoint) {
  return TerminalGlyphs::findIndex(codepoint) != TerminalGlyphs::NOT_FOUND;
}

void TerminalFont::drawGlyph(const GfxRenderer& renderer, const int x, const int y, const int cellWidth,
                             const uint16_t glyphIndex, const uint8_t attributes, const bool cursor) {
  const bool inverse = (attributes & TerminalScreen::ATTR_INVERSE) != 0;
  renderer.fillRect(x, y, cellWidth, CELL_HEIGHT, inverse);

  const uint8_t* const glyphRows = TerminalGlyphs::rows(glyphIndex);

  const bool ink = !inverse;
  const bool hidden = (attributes & TerminalScreen::ATTR_HIDDEN) != 0;
  const int glyphInset = (cellWidth - GLYPH_WIDTH) / 2;
  if (!hidden) {
    for (int glyphY = 0; glyphY < GLYPH_HEIGHT; ++glyphY) {
      const uint8_t bits = glyphRows[glyphY];
      for (int glyphX = 0; glyphX < GLYPH_WIDTH; ++glyphX) {
        if ((bits & (uint8_t{0x80} >> glyphX)) == 0) continue;
        const int pixelX = x + glyphInset + glyphX;
        const int pixelY = y + glyphY;
        renderer.drawPixel(pixelX, pixelY, ink);
        if ((attributes & TerminalScreen::ATTR_BOLD) != 0 && pixelX + 1 < x + cellWidth) {
          renderer.drawPixel(pixelX + 1, pixelY, ink);
        }
      }
    }
  }

  if (!hidden && (attributes & TerminalScreen::ATTR_UNDERLINE) != 0) {
    renderer.drawLine(x + glyphInset, y + CELL_HEIGHT - 2, x + cellWidth - 1, y + CELL_HEIGHT - 2, ink);
  }
  if (!hidden && (attributes & TerminalScreen::ATTR_STRIKETHROUGH) != 0) {
    renderer.drawLine(x + glyphInset, y + CELL_HEIGHT / 2, x + cellWidth - 1, y + CELL_HEIGHT / 2, ink);
  }
  if (cursor) {
    // Keep the character readable and drive only the native glyph's bottom row
    // instead of inverting the complete cell on every cursor movement.
    renderer.drawLine(x + glyphInset, y + CELL_HEIGHT - 1, x + glyphInset + GLYPH_WIDTH - 1, y + CELL_HEIGHT - 1, ink);
  }
}
