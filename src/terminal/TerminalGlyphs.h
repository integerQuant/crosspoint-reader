#pragma once

#include <cstdint>

namespace TerminalGlyphs {

using Index = uint16_t;

constexpr uint16_t MAX_GLYPHS = 2048;
constexpr Index NOT_FOUND = UINT16_MAX;
constexpr Index SPACE_INDEX = 0;

Index findIndex(uint16_t codepoint);
Index indexOrReplacement(uint16_t codepoint);
uint16_t codepoint(Index index);
const uint8_t* rows(Index index);
uint16_t count();

}  // namespace TerminalGlyphs
