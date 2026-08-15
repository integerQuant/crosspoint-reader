#include "TerminalScreen.h"

#include <algorithm>
#include <cstring>
#include <iterator>

TerminalScreen::TerminalScreen() { reset(); }

void TerminalScreen::reset() {
  for (auto& row : cells) {
    std::fill(std::begin(row), std::end(row), BLANK_CELL);
  }
  cursorRow = 0;
  cursorColumn = 0;
  currentAttributes = ATTR_NONE;
  cursorVisible = true;
  wrapPending = false;
  markAllDirty();
}

void TerminalScreen::markRowDirty(const uint8_t row) { dirtyRows |= uint32_t{1} << row; }

void TerminalScreen::markAllDirty() { dirtyRows = (uint32_t{1} << ROWS) - 1; }

uint32_t TerminalScreen::takeDirtyRows() {
  const uint32_t result = dirtyRows;
  dirtyRows = 0;
  return result;
}

void TerminalScreen::putCharacter(uint8_t character) {
  if (character < 0x20 || character > 0x7e) {
    character = '?';
  }

  // VT terminals defer wrapping until the next printable character. This is
  // essential for an exactly-full line followed by CRLF: eager wrapping plus
  // the explicit LF would otherwise consume two rows.
  if (wrapPending) {
    cursorColumn = 0;
    lineFeed();
  }

  markRowDirty(cursorRow);
  cells[cursorRow][cursorColumn] = Cell{character, currentAttributes};
  if (cursorColumn + 1 >= COLS) {
    wrapPending = true;
  } else {
    ++cursorColumn;
    markRowDirty(cursorRow);
  }
}

void TerminalScreen::cancelPendingWrap() { wrapPending = false; }

void TerminalScreen::scrollUp() {
  std::memmove(cells[0], cells[1], sizeof(Cell) * COLS * (ROWS - 1));
  std::fill(std::begin(cells[ROWS - 1]), std::end(cells[ROWS - 1]), BLANK_CELL);
  markAllDirty();
}

void TerminalScreen::lineFeed() {
  cancelPendingWrap();
  markRowDirty(cursorRow);
  if (cursorRow + 1 < ROWS) {
    ++cursorRow;
    markRowDirty(cursorRow);
  } else {
    scrollUp();
  }
}

void TerminalScreen::carriageReturn() {
  cancelPendingWrap();
  markRowDirty(cursorRow);
  cursorColumn = 0;
}

void TerminalScreen::backspace() {
  cancelPendingWrap();
  if (cursorColumn > 0) {
    markRowDirty(cursorRow);
    --cursorColumn;
  }
}

void TerminalScreen::tab() {
  cancelPendingWrap();
  const uint8_t spaces = static_cast<uint8_t>(TAB_WIDTH - (cursorColumn % TAB_WIDTH));
  for (uint8_t i = 0; i < spaces; ++i) {
    putCharacter(' ');
  }
}

void TerminalScreen::moveCursor(const int rowDelta, const int columnDelta) {
  cancelPendingWrap();
  markRowDirty(cursorRow);
  cursorRow = static_cast<uint8_t>(std::clamp(static_cast<int>(cursorRow) + rowDelta, 0, ROWS - 1));
  cursorColumn = static_cast<uint8_t>(std::clamp(static_cast<int>(cursorColumn) + columnDelta, 0, COLS - 1));
  markRowDirty(cursorRow);
}

void TerminalScreen::setCursor(const uint16_t oneBasedRow, const uint16_t oneBasedColumn) {
  cancelPendingWrap();
  markRowDirty(cursorRow);
  const uint16_t row = oneBasedRow == 0 ? 0 : oneBasedRow - 1;
  const uint16_t column = oneBasedColumn == 0 ? 0 : oneBasedColumn - 1;
  cursorRow = static_cast<uint8_t>(std::min<uint16_t>(row, ROWS - 1));
  cursorColumn = static_cast<uint8_t>(std::min<uint16_t>(column, COLS - 1));
  markRowDirty(cursorRow);
}

void TerminalScreen::setCursorVisible(const bool visible) {
  if (cursorVisible != visible) {
    cursorVisible = visible;
    markRowDirty(cursorRow);
  }
}

void TerminalScreen::clearRange(const uint8_t row, const uint8_t firstColumn, const uint8_t lastColumn) {
  std::fill(cells[row] + firstColumn, cells[row] + lastColumn + 1, BLANK_CELL);
  markRowDirty(row);
}

void TerminalScreen::clearDisplay(const uint16_t mode) {
  cancelPendingWrap();
  if (mode == 2 || mode == 3) {
    for (uint8_t row = 0; row < ROWS; ++row) {
      clearRange(row, 0, COLS - 1);
    }
    return;
  }

  if (mode == 1) {
    for (uint8_t row = 0; row < cursorRow; ++row) {
      clearRange(row, 0, COLS - 1);
    }
    clearRange(cursorRow, 0, cursorColumn);
    return;
  }

  clearRange(cursorRow, cursorColumn, COLS - 1);
  for (uint8_t row = cursorRow + 1; row < ROWS; ++row) {
    clearRange(row, 0, COLS - 1);
  }
}

void TerminalScreen::clearLine(const uint16_t mode) {
  cancelPendingWrap();
  if (mode == 1) {
    clearRange(cursorRow, 0, cursorColumn);
  } else if (mode == 2) {
    clearRange(cursorRow, 0, COLS - 1);
  } else {
    clearRange(cursorRow, cursorColumn, COLS - 1);
  }
}
