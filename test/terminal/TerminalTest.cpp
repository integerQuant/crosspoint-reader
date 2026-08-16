#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "TerminalFont.h"
#include "TerminalLayout.h"
#include "TerminalParser.h"
#include "TerminalProtocol.h"
#include "TerminalRenderGate.h"
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
    result.push_back(static_cast<char>(screen.getCell(row, column).codepoint));
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

  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'C');
  EXPECT_EQ(screen.getCell(TerminalScreen::ROWS - 2, 0).codepoint, static_cast<uint8_t>('A' + TerminalScreen::ROWS));
  EXPECT_EQ(screen.getCursorRow(), TerminalScreen::ROWS - 1);
}

TEST(TerminalScreenTest, HandlesControlsTabsAndDirtyRows) {
  TerminalScreen screen;
  TerminalParser parser(screen);
  screen.takeDirtyRegion();

  feed(parser, "a\tb\bX\rZ");

  EXPECT_EQ(rowText(screen, 0, 10), "Z       X ");
  const auto dirty = screen.takeDirtyRegion();
  EXPECT_EQ(dirty.rows, 1u);
  EXPECT_EQ(dirty.firstColumn[0], 0);
  EXPECT_EQ(dirty.lastColumn[0], 9);
  EXPECT_TRUE(screen.takeDirtyRegion().empty());
}

TEST(TerminalScreenTest, DefersWrapUntilTheNextPrintableCharacter) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, std::string(TerminalScreen::COLS, 'A'));
  EXPECT_EQ(screen.getCursorRow(), 0);
  EXPECT_EQ(screen.getCursorColumn(), TerminalScreen::COLS - 1);

  feed(parser, "B");
  EXPECT_EQ(screen.getCell(1, 0).codepoint, 'B');
  EXPECT_EQ(screen.getCursorRow(), 1);
  EXPECT_EQ(screen.getCursorColumn(), 1);
}

TEST(TerminalScreenTest, FullWidthCrLfDoesNotInsertABlankLine) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, std::string(TerminalScreen::COLS, 'A'));
  feed(parser, "\r\nB");

  EXPECT_EQ(screen.getCell(0, TerminalScreen::COLS - 1).codepoint, 'A');
  EXPECT_EQ(screen.getCell(1, 0).codepoint, 'B');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, ' ');
  EXPECT_EQ(screen.getCursorRow(), 1);
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
  EXPECT_EQ(styled.codepoint, 'Z');
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
  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'A');
  EXPECT_EQ(screen.getCell(0, 0).attributes, TerminalScreen::ATTR_NONE);

  feed(parser, "\x1b[12\x1b[xB");
  EXPECT_EQ(screen.getCell(0, 1).codepoint, 'B');
}

TEST(TerminalParserTest, DecodesUtf8IntoSingleCells) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "\xc3\xa9\xe2\x94\x80\xee\x82\xb0");

  EXPECT_EQ(screen.getCell(0, 0).codepoint, 0x00e9);
  EXPECT_EQ(screen.getCell(0, 1).codepoint, 0x2500);
  EXPECT_EQ(screen.getCell(0, 2).codepoint, 0xe0b0);
  EXPECT_EQ(screen.getCursorColumn(), 3);
}

TEST(TerminalParserTest, InvalidAndNonBmpUtf8UseOneReplacementCell) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser,
       "\xf0\x9f\x98\x80"
       "A");

  EXPECT_EQ(screen.getCell(0, 0).codepoint, TerminalScreen::REPLACEMENT_CODEPOINT);
  EXPECT_EQ(screen.getCell(0, 1).codepoint, 'A');
  EXPECT_EQ(screen.getCursorColumn(), 2);
}

TEST(TerminalScreenTest, LineFeedAdvancesExactlyOneRow) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "one\ntwo\r\nthree");

  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'o');
  EXPECT_EQ(screen.getCell(1, 3).codepoint, 't');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, 't');
  EXPECT_EQ(screen.getCursorRow(), 2);
}

TEST(TerminalLayoutTest, PreservesInsetAndAllEightyColumns) {
  EXPECT_EQ(TerminalLayout::columnX(0), 8);
  EXPECT_EQ(TerminalLayout::columnX(TerminalScreen::COLS), 800);

  int compressedCells = 0;
  for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
    const int width = TerminalLayout::columnWidth(column);
    EXPECT_GE(width, TerminalFont::GLYPH_WIDTH);
    EXPECT_LE(width, TerminalFont::CELL_WIDTH);
    if (width == 9) ++compressedCells;
  }
  EXPECT_EQ(compressedCells, 8);
}

TEST(TerminalLayoutTest, DirtySpanUsesTheSameVariableCellGeometry) {
  EXPECT_EQ(TerminalLayout::spanWidth(0, 0), TerminalLayout::columnWidth(0));
  EXPECT_EQ(TerminalLayout::spanWidth(9, 10), TerminalLayout::columnWidth(9) + TerminalLayout::columnWidth(10));
  EXPECT_EQ(TerminalLayout::spanWidth(0, TerminalScreen::COLS - 1), 792);
}

TEST(TerminalRenderGateTest, SchedulesIdleRequestsImmediately) {
  TerminalRenderGate gate;

  EXPECT_TRUE(gate.request());
  EXPECT_FALSE(gate.complete());
  EXPECT_TRUE(gate.request());
  EXPECT_FALSE(gate.complete());
}

TEST(TerminalRenderGateTest, ReplaysARequestMadeDuringRender) {
  TerminalRenderGate gate;

  EXPECT_TRUE(gate.request());
  EXPECT_FALSE(gate.request());
  EXPECT_TRUE(gate.complete());
  EXPECT_FALSE(gate.complete());
}

TEST(TerminalRenderGateTest, CoalescesMultipleRequestsIntoOneReplay) {
  TerminalRenderGate gate;

  EXPECT_TRUE(gate.request());
  EXPECT_FALSE(gate.request());
  EXPECT_FALSE(gate.request());
  EXPECT_TRUE(gate.complete());
  EXPECT_FALSE(gate.complete());
}

TEST(TerminalProtocolTest, EncodesTheGoldenHeaderInNetworkOrder) {
  std::array<uint8_t, knietty::FRAME_HEADER_SIZE> header{};
  knietty::encodeFrameHeader(header.data(), static_cast<uint8_t>(knietty::FrameType::TerminalOutput), 0, 3, 0x01020304);

  constexpr std::array<uint8_t, knietty::FRAME_HEADER_SIZE> expected{0x01, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04};
  EXPECT_EQ(header, expected);
}

TEST(TerminalProtocolTest, DecodesFragmentedGoldenFrame) {
  constexpr std::array<uint8_t, 11> encoded{0x01, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 'a', 'b', 'c'};
  knietty::FrameDecoder decoder;
  for (size_t index = 0; index < encoded.size(); ++index) {
    const auto result = decoder.feed(encoded[index]);
    EXPECT_EQ(result, index + 1 == encoded.size() ? knietty::FrameDecoder::FeedResult::Ready
                                                  : knietty::FrameDecoder::FeedResult::NeedMore);
  }

  const auto frame = decoder.frame();
  EXPECT_EQ(frame.type, static_cast<uint8_t>(knietty::FrameType::TerminalOutput));
  EXPECT_EQ(frame.flags, 0);
  EXPECT_EQ(frame.length, 3);
  EXPECT_EQ(frame.sequence, 0x01020304u);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(frame.payload), frame.length), "abc");
}

TEST(TerminalProtocolTest, ConsumesBackToBackFramesWithoutStateLeakage) {
  knietty::FrameDecoder decoder;
  constexpr std::array<uint8_t, 8> emptyHeartbeat{0x06, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
  for (const uint8_t byte : emptyHeartbeat) decoder.feed(byte);
  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.frame().sequence, UINT32_MAX);
  decoder.consume();

  constexpr std::array<uint8_t, 9> input{0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03};
  for (const uint8_t byte : input) decoder.feed(byte);
  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.frame().length, 1);
  EXPECT_EQ(decoder.frame().payload[0], 0x03);
}

TEST(TerminalProtocolTest, RejectsFlagsAndOversizedPayloadBeforeReadingPayload) {
  knietty::FrameDecoder flagsDecoder;
  constexpr std::array<uint8_t, 8> flags{0x01, 0x01, 0x00, 0x00, 0, 0, 0, 1};
  for (const uint8_t byte : flags) flagsDecoder.feed(byte);
  EXPECT_EQ(flagsDecoder.getError(), knietty::FrameError::InvalidFlags);

  knietty::FrameDecoder lengthDecoder;
  constexpr std::array<uint8_t, 8> oversized{0x01, 0x00, 0x02, 0x01, 0, 0, 0, 1};
  for (const uint8_t byte : oversized) lengthDecoder.feed(byte);
  EXPECT_EQ(lengthDecoder.getError(), knietty::FrameError::PayloadTooLarge);
}

TEST(TerminalProtocolTest, DistinguishesKnownAndOptionalTypes) {
  EXPECT_TRUE(knietty::isKnownFrameType(static_cast<uint8_t>(knietty::FrameType::RefreshEvent)));
  EXPECT_FALSE(knietty::isKnownFrameType(0x07));
  EXPECT_TRUE(knietty::isOptionalFrameType(0x80));
  EXPECT_FALSE(knietty::isOptionalFrameType(0x07));
}

}  // namespace
