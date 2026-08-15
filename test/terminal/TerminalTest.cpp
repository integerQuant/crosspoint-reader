#include <gtest/gtest.h>

#include <string_view>

#include "TerminalParser.h"
#include "TerminalScreen.h"

namespace {

void feed(TerminalParser& parser, const std::string_view bytes) {
  for (const unsigned char byte : bytes) {
    parser.feed(byte);
  }
}

std::string rowText(const TerminalScreen& screen, const uint8_t row, const uint8_t count) {
  std::string result;
  for (uint8_t column = 0; column < count; ++column) {
    result.push_back(static_cast<char>(screen.getCell(row, column).character));
  }
  return result;
}

TEST(TerminalScreenTest, WrapsAndScrollsWithinFixedBounds) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  for (uint8_t row = 0; row < TerminalScreen::ROWS + 1; ++row) {
    feed(parser, std::string(1, static_cast<char>('A' + row)));
    feed(parser, "\r\n");
  }

  EXPECT_EQ(screen.getCell(0, 0).character, 'C');
  EXPECT_EQ(screen.getCell(TerminalScreen::ROWS - 2, 0).character, static_cast<uint8_t>('A' + TerminalScreen::ROWS));
  EXPECT_EQ(screen.getCursorRow(), TerminalScreen::ROWS - 1);
}

TEST(TerminalScreenTest, HandlesControlsTabsAndDirtyRows) {
  TerminalScreen screen;
  TerminalParser parser(screen);
  screen.takeDirtyRows();

  feed(parser, "a\tb\bX\rZ");

  EXPECT_EQ(rowText(screen, 0, 10), "Z       X ");
  EXPECT_EQ(screen.takeDirtyRows(), 1u);
  EXPECT_EQ(screen.takeDirtyRows(), 0u);
}

TEST(TerminalParserTest, MovesCursorAndClearsLines) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "one\r\ntwo");
  feed(parser, "\x1b[1A\x1b[1DX\x1b[K");

  EXPECT_EQ(rowText(screen, 0, 4), "onX ");
  EXPECT_EQ(rowText(screen, 1, 3), "two");
}

TEST(TerminalParserTest, SupportsPositionClearAndMonochromeAttributes) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "abc\x1b[2;3H\x1b[1;4;7mZ\x1b[0mN");
  const auto styled = screen.getCell(1, 2);
  EXPECT_EQ(styled.character, 'Z');
  EXPECT_EQ(styled.attributes,
            TerminalScreen::ATTR_BOLD | TerminalScreen::ATTR_UNDERLINE | TerminalScreen::ATTR_INVERSE);
  EXPECT_EQ(screen.getCell(1, 3).attributes, TerminalScreen::ATTR_NONE);

  feed(parser, "\x1b[2J");
  EXPECT_EQ(rowText(screen, 0, 3), "   ");
  EXPECT_EQ(rowText(screen, 1, 4), "    ");
}

TEST(TerminalParserTest, TogglesCursorAndIgnoresColorsAndMalformedSequences) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "\x1b[?25l\x1b[31;47mA\x1b[?25h");
  EXPECT_TRUE(screen.isCursorVisible());
  EXPECT_EQ(screen.getCell(0, 0).character, 'A');
  EXPECT_EQ(screen.getCell(0, 0).attributes, TerminalScreen::ATTR_NONE);

  feed(parser, "\x1b[12\x1b[xB");
  EXPECT_EQ(screen.getCell(0, 1).character, 'B');
}

}  // namespace
