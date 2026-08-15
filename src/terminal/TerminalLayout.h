#pragma once

#include <cstdint>

namespace TerminalLayout {

constexpr uint8_t COLUMNS = 80;
constexpr int SCREEN_WIDTH = 800;
constexpr int LEFT_INSET = 4;
constexpr int BASE_CELL_WIDTH = 10;
constexpr int COMPRESSED_CELL_INTERVAL = 20;
constexpr int TOP = 40;
constexpr int CELL_HEIGHT = 18;

// Four pixels at the physical left edge are hidden by the X4 bezel. Preserve
// all 80 columns by taking one spacing pixel from every twentieth cell. Glyphs
// remain their native 8 pixels wide; only the otherwise-empty cell gutter is
// compressed.
constexpr int columnX(const uint8_t column) {
  return LEFT_INSET + static_cast<int>(column) * BASE_CELL_WIDTH - static_cast<int>(column) / COMPRESSED_CELL_INTERVAL;
}

constexpr int columnWidth(const uint8_t column) { return columnX(column + 1) - columnX(column); }

constexpr int spanWidth(const uint8_t firstColumn, const uint8_t lastColumn) {
  return columnX(lastColumn + 1) - columnX(firstColumn);
}

static_assert(columnX(0) == LEFT_INSET);
static_assert(columnX(COLUMNS) == SCREEN_WIDTH);
static_assert(columnWidth(19) == 9);
static_assert(columnWidth(20) == 10);

}  // namespace TerminalLayout
