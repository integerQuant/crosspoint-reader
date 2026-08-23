#pragma once

#include <cstdint>

#include "TerminalFont.h"
#include "TerminalScreen.h"

namespace TerminalLayout {

constexpr uint8_t COLUMNS = TerminalScreen::COLS;
constexpr int SCREEN_WIDTH = 800;
constexpr int LEFT_INSET = 6;
constexpr int CELL_WIDTH = TerminalFont::CELL_WIDTH;
constexpr int TOP = 32;
constexpr int CELL_HEIGHT = TerminalFont::CELL_HEIGHT;

// Use the native 8x16 Terminus cell without an added gutter. The six-pixel
// inset protects the X4's obscured left edge; 99 cells end at x=798 and retain
// two physical pixels on the right.
constexpr int columnX(const uint8_t column) { return LEFT_INSET + static_cast<int>(column) * CELL_WIDTH; }

constexpr int columnWidth(const uint8_t) { return CELL_WIDTH; }

constexpr int spanWidth(const uint8_t firstColumn, const uint8_t lastColumn) {
  return columnX(lastColumn + 1) - columnX(firstColumn);
}

static_assert(columnX(0) == LEFT_INSET);
static_assert(columnX(COLUMNS) == SCREEN_WIDTH - 2);
static_assert(columnWidth(0) == CELL_WIDTH);
static_assert(TOP + TerminalScreen::ROWS * CELL_HEIGHT == 480);

}  // namespace TerminalLayout
