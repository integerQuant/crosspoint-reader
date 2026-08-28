#include "TerminalGlyphs.h"

#if defined(KNIETTY_FONT_TERMINUS)
#include "TerminalFontData.terminus.generated.h"
#elif defined(KNIETTY_FONT_UNIFONT)
#include "TerminalFontData.unifont.generated.h"
#else
#include "TerminalFontData.generated.h"
#endif

namespace {

constexpr uint16_t REPLACEMENT_CODEPOINT = 0xfffd;
constexpr uint16_t QUESTION_CODEPOINT = '?';

}  // namespace

TerminalGlyphs::Index TerminalGlyphs::findIndex(const uint16_t codepoint) {
  uint16_t first = 0;
  uint16_t remaining = TerminalFontData::GLYPH_COUNT;
  while (remaining > 0) {
    const uint16_t step = remaining / 2;
    const uint16_t index = first + step;
    if (TerminalFontData::GLYPHS[index].codepoint < codepoint) {
      first = index + 1;
      remaining -= step + 1;
    } else {
      remaining = step;
    }
  }
  if (first < TerminalFontData::GLYPH_COUNT && TerminalFontData::GLYPHS[first].codepoint == codepoint) return first;
  return NOT_FOUND;
}

TerminalGlyphs::Index TerminalGlyphs::indexOrReplacement(const uint16_t codepoint) {
  const Index direct = findIndex(codepoint);
  if (direct != NOT_FOUND) return direct;
  const Index replacement = findIndex(REPLACEMENT_CODEPOINT);
  if (replacement != NOT_FOUND) return replacement;
  return findIndex(QUESTION_CODEPOINT);
}

uint16_t TerminalGlyphs::codepoint(const Index index) {
  return index < TerminalFontData::GLYPH_COUNT ? TerminalFontData::GLYPHS[index].codepoint : REPLACEMENT_CODEPOINT;
}

const uint8_t* TerminalGlyphs::rows(const Index index) {
  const Index safeIndex = index < TerminalFontData::GLYPH_COUNT ? index : indexOrReplacement(REPLACEMENT_CODEPOINT);
  return TerminalFontData::GLYPHS[safeIndex].rows;
}

uint16_t TerminalGlyphs::count() { return TerminalFontData::GLYPH_COUNT; }

static_assert(TerminalFontData::GLYPH_COUNT <= TerminalGlyphs::MAX_GLYPHS);
static_assert(TerminalFontData::GLYPHS[TerminalGlyphs::SPACE_INDEX].codepoint == ' ');
