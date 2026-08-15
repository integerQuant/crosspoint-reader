#include "TerminalFont.h"

#include <GfxRenderer.h>

#include "TerminalFontData.generated.h"
#include "TerminalScreen.h"

namespace {

const TerminalFontData::Glyph* findGlyph(const uint16_t codepoint) {
  uint16_t first = 0;
  uint16_t count = TerminalFontData::GLYPH_COUNT;
  while (count > 0) {
    const uint16_t step = count / 2;
    const uint16_t index = first + step;
    if (TerminalFontData::GLYPHS[index].codepoint < codepoint) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  if (first < TerminalFontData::GLYPH_COUNT && TerminalFontData::GLYPHS[first].codepoint == codepoint) {
    return &TerminalFontData::GLYPHS[first];
  }
  return nullptr;
}

}  // namespace

bool TerminalFont::hasGlyph(const uint16_t codepoint) { return findGlyph(codepoint) != nullptr; }

void TerminalFont::drawCell(const GfxRenderer& renderer, const int x, const int y, const uint16_t codepoint,
                            const uint8_t attributes, const bool cursor) {
  const bool inverse = ((attributes & TerminalScreen::ATTR_INVERSE) != 0) != cursor;
  renderer.fillRect(x, y, CELL_WIDTH, CELL_HEIGHT, inverse);

  const auto* glyph = findGlyph(codepoint);
  if (glyph == nullptr) glyph = findGlyph('?');

  const bool ink = !inverse;
  for (int glyphY = 0; glyphY < GLYPH_HEIGHT; ++glyphY) {
    const uint8_t bits = glyph->rows[glyphY];
    for (int glyphX = 0; glyphX < GLYPH_WIDTH; ++glyphX) {
      if ((bits & (uint8_t{0x80} >> glyphX)) == 0) continue;
      const int pixelX = x + 1 + glyphX;
      const int pixelY = y + 1 + glyphY;
      renderer.drawPixel(pixelX, pixelY, ink);
      if ((attributes & TerminalScreen::ATTR_BOLD) != 0 && pixelX + 1 < x + CELL_WIDTH - 1) {
        renderer.drawPixel(pixelX + 1, pixelY, ink);
      }
    }
  }

  if ((attributes & TerminalScreen::ATTR_UNDERLINE) != 0) {
    renderer.drawLine(x + 1, y + CELL_HEIGHT - 2, x + CELL_WIDTH - 2, y + CELL_HEIGHT - 2, ink);
  }
}
