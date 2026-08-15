#pragma once

#include <cstddef>
#include <cstdint>

class TerminalScreen {
 public:
  static constexpr uint8_t COLS = 50;
  static constexpr uint8_t ROWS = 22;
  static constexpr uint8_t TAB_WIDTH = 8;

  enum Attribute : uint8_t {
    ATTR_NONE = 0,
    ATTR_BOLD = 1 << 0,
    ATTR_INVERSE = 1 << 1,
    ATTR_UNDERLINE = 1 << 2,
  };

  struct Cell {
    uint8_t character;
    uint8_t attributes;
  };

  TerminalScreen();

  void reset();
  void putCharacter(uint8_t character);
  void lineFeed();
  void carriageReturn();
  void backspace();
  void tab();

  void moveCursor(int rowDelta, int columnDelta);
  void setCursor(uint16_t oneBasedRow, uint16_t oneBasedColumn);
  void setCursorVisible(bool visible);
  void clearDisplay(uint16_t mode);
  void clearLine(uint16_t mode);

  void setAttributes(uint8_t attributes) { currentAttributes = attributes; }
  uint8_t getAttributes() const { return currentAttributes; }
  bool isCursorVisible() const { return cursorVisible; }
  uint8_t getCursorRow() const { return cursorRow; }
  uint8_t getCursorColumn() const { return cursorColumn; }
  const Cell& getCell(uint8_t row, uint8_t column) const { return cells[row][column]; }

  uint32_t takeDirtyRows();
  void markAllDirty();

 private:
  Cell cells[ROWS][COLS];
  uint8_t cursorRow = 0;
  uint8_t cursorColumn = 0;
  uint8_t currentAttributes = ATTR_NONE;
  bool cursorVisible = true;
  uint32_t dirtyRows = 0;

  static constexpr Cell BLANK_CELL{' ', ATTR_NONE};

  void markRowDirty(uint8_t row);
  void scrollUp();
  void clearRange(uint8_t row, uint8_t firstColumn, uint8_t lastColumn);
};
