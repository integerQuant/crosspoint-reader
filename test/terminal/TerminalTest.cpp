#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "TerminalDiagnostics.h"
#include "TerminalFont.h"
#include "TerminalFontData.terminus.generated.h"
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

size_t captureReply(void* const context, const uint8_t* const data, const size_t length) {
  static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(data), length);
  return length;
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

TEST(TerminalScreenTest, PrunesTuiClearAndRepaintWhenFinalFrameIsUnchanged) {
  TerminalScreen previous;
  TerminalParser previousParser(previous);
  feed(previousParser, "\x1b[?25lHELLO");
  previous.takeDirtyRegion();

  TerminalScreen current = previous;
  TerminalParser currentParser(current);
  feed(currentParser, "\x1b[1;1H\x1b[2KHELLO");

  EXPECT_TRUE(current.takeDirtyRegionComparedTo(previous).empty());
}

TEST(TerminalScreenTest, VisualDiffRetainsOldAndNewCursorCells) {
  TerminalScreen previous;
  TerminalParser previousParser(previous);
  feed(previousParser, "A");
  previous.takeDirtyRegion();

  TerminalScreen current = previous;
  TerminalParser currentParser(current);
  feed(currentParser, "\x1b[1D");

  const auto dirty = current.takeDirtyRegionComparedTo(previous);
  EXPECT_EQ(dirty.rows, 1u);
  EXPECT_EQ(dirty.firstColumn[0], 0);
  EXPECT_EQ(dirty.lastColumn[0], 1);
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

TEST(TerminalParserTest, ConsumesCapturedCodexControlStringsAndCursorStyle) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser,
       "\x1b]0;private title\x07"
       "A"
       "\x1b[0 q"
       "\x1b]10;ignored\x1b\\"
       "\x1bPignored dcs\x1b\\"
       "\x1b^ignored pm\x1b\\"
       "\x1b_ignored apc\x1b\\"
       "\x1b(0"
       "\x1b[>5u"
       "\x1b[<1u"
       "\x1b[?u"
       "B");

  EXPECT_EQ(rowText(screen, 0, 4), "AB  ");
  EXPECT_EQ(screen.getCursorColumn(), 2);
}

TEST(TerminalParserTest, ScrollsOnlyInsideCapturedCodexMargins) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  for (uint8_t row = 0; row < 5; ++row) {
    screen.setCursor(row + 1, 1);
    screen.putCodepoint(static_cast<uint8_t>('A' + row));
  }

  feed(parser, "\x1b[2;4r\x1b[4;1H\n");
  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'A');
  EXPECT_EQ(screen.getCell(1, 0).codepoint, 'C');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, 'D');
  EXPECT_EQ(screen.getCell(3, 0).codepoint, ' ');
  EXPECT_EQ(screen.getCell(4, 0).codepoint, 'E');

  feed(parser, "\x1b[2;1H\x1bM");
  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'A');
  EXPECT_EQ(screen.getCell(1, 0).codepoint, ' ');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, 'C');
  EXPECT_EQ(screen.getCell(3, 0).codepoint, 'D');
  EXPECT_EQ(screen.getCell(4, 0).codepoint, 'E');

  feed(parser, "\x1b[1S");
  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'A');
  EXPECT_EQ(screen.getCell(1, 0).codepoint, 'C');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, 'D');
  EXPECT_EQ(screen.getCell(3, 0).codepoint, ' ');
  EXPECT_EQ(screen.getCell(4, 0).codepoint, 'E');
}

TEST(TerminalParserTest, SavesAndRestoresCursorForCapturedCodexSequence) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  const std::string sequence =
      "A\x1b"
      "7\x1b[" +
      std::to_string(TerminalScreen::ROWS) + ";" + std::to_string(TerminalScreen::COLS) +
      "HZ\x1b"
      "8B";
  feed(parser, sequence);

  EXPECT_EQ(rowText(screen, 0, 3), "AB ");
  EXPECT_EQ(screen.getCell(TerminalScreen::ROWS - 1, TerminalScreen::COLS - 1).codepoint, 'Z');
  EXPECT_EQ(screen.getCursorRow(), 0);
  EXPECT_EQ(screen.getCursorColumn(), 2);
}

TEST(TerminalParserTest, AutoWrapScrollsInsideCapturedCodexMargins) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  for (uint8_t row = 0; row < 5; ++row) {
    screen.setCursor(row + 1, 1);
    screen.putCodepoint(static_cast<uint8_t>('A' + row));
  }

  feed(parser, "\x1b[2;4r\x1b[4;" + std::to_string(TerminalScreen::COLS) + "HXY");

  EXPECT_EQ(screen.getCell(0, 0).codepoint, 'A');
  EXPECT_EQ(screen.getCell(1, 0).codepoint, 'C');
  EXPECT_EQ(screen.getCell(2, 0).codepoint, 'D');
  EXPECT_EQ(screen.getCell(2, TerminalScreen::COLS - 1).codepoint, 'X');
  EXPECT_EQ(screen.getCell(3, 0).codepoint, 'Y');
  EXPECT_EQ(screen.getCell(4, 0).codepoint, 'E');
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

TEST(TerminalParserTest, SupportsModernTuiEditingOperations) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "ABCDE\x1b[1;3H\x1b[2@XY");
  EXPECT_EQ(rowText(screen, 0, 9), "ABXYCDE  ");
  feed(parser, "\x1b[1;3H\x1b[2P");
  EXPECT_EQ(rowText(screen, 0, 7), "ABCDE  ");
  feed(parser, "\x1b[1;3H\x1b[2X");
  EXPECT_EQ(rowText(screen, 0, 5), "AB  E");

  feed(parser, "\x1b[2;5H\x1b[3EZ\x1b[3b");
  EXPECT_EQ(screen.getCursorRow(), 4);
  EXPECT_EQ(rowText(screen, 4, 5), "ZZZZ ");
  feed(parser, "\x1b[2F\x1b[7G\x1b[9d");
  EXPECT_EQ(screen.getCursorRow(), 8);
  EXPECT_EQ(screen.getCursorColumn(), 6);
}

TEST(TerminalParserTest, InsertsAndDeletesLinesInsideMargins) {
  TerminalScreen screen;
  TerminalParser parser(screen);
  for (uint8_t row = 0; row < 4; ++row) {
    screen.setCursor(row + 1, 1);
    screen.putCodepoint(static_cast<uint8_t>('A' + row));
  }

  feed(parser, "\x1b[2;4r\x1b[3;1H\x1b[L");
  EXPECT_EQ(rowText(screen, 0, 1), "A");
  EXPECT_EQ(rowText(screen, 1, 1), "B");
  EXPECT_EQ(rowText(screen, 2, 1), " ");
  EXPECT_EQ(rowText(screen, 3, 1), "C");

  feed(parser, "\x1b[M");
  EXPECT_EQ(rowText(screen, 2, 1), "C");
  EXPECT_EQ(rowText(screen, 3, 1), " ");
}

TEST(TerminalParserTest, SkipsExtendedColorComponentsWithoutLeakingAttributes) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser, "\x1b[38;2;1;4;7;48;5;9mA\x1b[0m\x1b[38:2::1:4:7;1mB");
  EXPECT_EQ(screen.getCell(0, 0).attributes, TerminalScreen::ATTR_NONE);
  EXPECT_EQ(screen.getCell(0, 1).attributes, TerminalScreen::ATTR_BOLD);

  feed(parser, "\x1b[0;8;9mC\x1b[28;29mD");
  EXPECT_EQ(screen.getCell(0, 2).attributes, TerminalScreen::ATTR_HIDDEN | TerminalScreen::ATTR_STRIKETHROUGH);
  EXPECT_EQ(screen.getCell(0, 3).attributes, TerminalScreen::ATTR_NONE);
}

TEST(TerminalParserTest, IgnoresSelectorsAndJoinersWithoutConsumingCells) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser,
       "A\xef\xb8\x8f\xe2\x80\x8d\xe2\x80\x8b\xe2\x80\x8c"
       "B");

  EXPECT_EQ(rowText(screen, 0, 3), "AB ");
  EXPECT_EQ(screen.getCursorColumn(), 2);
}

TEST(TerminalParserTest, IgnoresDeleteControlCharacter) {
  TerminalScreen screen;
  TerminalParser parser(screen);

  feed(parser,
       "A\x7f"
       "B");

  EXPECT_EQ(rowText(screen, 0, 3), "AB ");
  EXPECT_EQ(screen.getCursorColumn(), 2);
}

TEST(TerminalParserTest, RepliesWithOnlyNativeLiteCapabilities) {
  TerminalScreen screen;
  std::string replies;
  TerminalParser parser(screen, {&replies, &captureReply});

  feed(parser, "\x1b[4;7H\x1b[5n\x1b[6n\x1b[c\x1b[>q\x1b[?2026$p");
  EXPECT_EQ(replies,
            "\x1b[0n"
            "\x1b[4;7R"
            "\x1b[?1;0c"
            "\x1bP>|knietty 0.1.3\x1b\\"
            "\x1b[?2026;2$y");

  replies.clear();
  feed(parser,
       "\x1b[?2026h\x1b[?2026$p"
       "\x1b[?u"
       "\x1bP+q4d73\x1b\\"
       "\x1b]10;?\x07"
       "\x1b]11;?\x1b\\"
       "\x1b_Ga=T,f=24,s=1,v=1;AAAA\x1b\\");
  EXPECT_EQ(replies,
            "\x1b[?2026;1$y"
            "\x1bP0+r4d73\x1b\\"
            "\x1b]10;rgb:0000/0000/0000\x1b\\"
            "\x1b]11;rgb:ffff/ffff/ffff\x1b\\");
}

TEST(TerminalParserTest, HoldsSynchronizedOutputUntilBoundaryOrWatchdogRelease) {
  TerminalScreen screen;
  TerminalParser parser(screen);
  screen.takeDirtyRegion();

  feed(parser, "\x1b[?2026hFRAME");
  EXPECT_TRUE(parser.isPresentationHeld());
  EXPECT_FALSE(parser.takePresentationBoundary());
  EXPECT_TRUE(screen.hasDirtyRows());

  feed(parser, "\x1b[?2026l");
  EXPECT_FALSE(parser.isPresentationHeld());
  EXPECT_TRUE(parser.takePresentationBoundary());
  EXPECT_FALSE(parser.takePresentationBoundary());

  feed(parser, "\x1b[?2026hNEXT");
  parser.releasePresentationHold();
  EXPECT_FALSE(parser.isPresentationHeld());
  EXPECT_TRUE(parser.takePresentationBoundary());
}

TEST(TerminalParserTest, ClearsOnIdempotentAlternateScreenTransitions) {
  TerminalScreen screen;
  TerminalParser parser(screen);
  feed(parser, "PRIMARY\x1b[?1049hALT");

  EXPECT_TRUE(parser.isAlternateScreen());
  EXPECT_EQ(rowText(screen, 0, 4), "ALT ");
  feed(parser, "\x1b[?1049h");
  EXPECT_EQ(rowText(screen, 0, 4), "ALT ");

  feed(parser, "\x1b[?1049l");
  EXPECT_FALSE(parser.isAlternateScreen());
  EXPECT_EQ(rowText(screen, 0, 4), "    ");
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

TEST(TerminalLayoutTest, UsesNativeCellsInsideThePhysicalEdges) {
  EXPECT_EQ(TerminalLayout::COLUMNS, TerminalScreen::COLS);
  EXPECT_EQ(TerminalLayout::columnX(0), 6);
  EXPECT_EQ(TerminalLayout::columnX(TerminalScreen::COLS), 798);
  EXPECT_EQ(TerminalLayout::TOP, 32);
  EXPECT_EQ(TerminalLayout::TOP + TerminalScreen::ROWS * TerminalLayout::CELL_HEIGHT, 480);

  int totalWidth = 0;
  for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
    const int width = TerminalLayout::columnWidth(column);
    EXPECT_EQ(width, TerminalFont::GLYPH_WIDTH);
    EXPECT_EQ(width, TerminalFont::CELL_WIDTH);
    totalWidth += width;
  }
  EXPECT_EQ(totalWidth, 792);
  EXPECT_EQ(TerminalLayout::CELL_HEIGHT, TerminalFont::GLYPH_HEIGHT);
  EXPECT_EQ(TerminalLayout::CELL_HEIGHT, TerminalFont::CELL_HEIGHT);
}

TEST(TerminalLayoutTest, DirtySpanUsesNativeCellGeometry) {
  EXPECT_EQ(TerminalLayout::spanWidth(0, 0), TerminalLayout::columnWidth(0));
  EXPECT_EQ(TerminalLayout::spanWidth(9, 10), TerminalLayout::columnWidth(9) + TerminalLayout::columnWidth(10));
  EXPECT_EQ(TerminalLayout::spanWidth(0, TerminalScreen::COLS - 1), 792);
}

TEST(TerminalFontTest, NativeLiteSupplementStaysInsideLookupBudget) {
  constexpr std::array<uint16_t, 27> expected{
      0x2139, 0x21b3, 0x21c6, 0x2299, 0x22ef, 0x25a3, 0x25b3, 0x25b6, 0x25b8, 0x25be, 0x25c8, 0x25c9, 0x2699, 0x26a0,
      0x2713, 0x2715, 0x2716, 0x2717, 0x2726, 0x2731, 0x276f, 0x27f3, 0x2b16, 0x2b1d, 0x2b25, 0x2b29, 0x2b2a,
  };
  EXPECT_EQ(TerminalFontData::GLYPH_COUNT, 964);
  EXPECT_LE(TerminalFontData::GLYPH_COUNT, 1023);
  for (const uint16_t codepoint : expected) {
    const auto* const glyph = std::lower_bound(
        std::begin(TerminalFontData::GLYPHS), std::end(TerminalFontData::GLYPHS), codepoint,
        [](const TerminalFontData::Glyph& item, const uint16_t value) { return item.codepoint < value; });
    ASSERT_NE(glyph, std::end(TerminalFontData::GLYPHS));
    EXPECT_EQ(glyph->codepoint, codepoint);
  }
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

TEST(TerminalProtocolTest, ReportsBoundedReadSizesAcrossHeaderAndPayload) {
  constexpr std::array<uint8_t, 11> encoded{0x01, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 'a', 'b', 'c'};
  knietty::FrameDecoder decoder;

  EXPECT_EQ(decoder.bytesNeeded(), knietty::FRAME_HEADER_SIZE);
  for (size_t index = 0; index < knietty::FRAME_HEADER_SIZE; ++index) decoder.feed(encoded[index]);
  EXPECT_EQ(decoder.bytesNeeded(), 3u);
  decoder.feed(encoded[8]);
  EXPECT_EQ(decoder.bytesNeeded(), 2u);
  decoder.feed(encoded[9]);
  decoder.feed(encoded[10]);
  EXPECT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.bytesNeeded(), 0u);
  decoder.consume();
  EXPECT_EQ(decoder.bytesNeeded(), knietty::FRAME_HEADER_SIZE);
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
  EXPECT_TRUE(knietty::isKnownFrameType(static_cast<uint8_t>(knietty::FrameType::SessionEnd)));
  EXPECT_TRUE(knietty::isKnownFrameType(static_cast<uint8_t>(knietty::FrameType::TerminalOutputEnd)));
  EXPECT_FALSE(knietty::isKnownFrameType(0x08));
  EXPECT_TRUE(knietty::isOptionalFrameType(static_cast<uint8_t>(knietty::FrameType::TerminalOutputEnd)));
  EXPECT_FALSE(knietty::isOptionalFrameType(0x07));
}

TEST(TerminalDiagnosticsTest, ValidatesTheBoundedCommandWhitelist) {
  using namespace knietty::diagnostics;
  Request request;
  constexpr std::array<uint8_t, 3> row{static_cast<uint8_t>(Command::Pattern), static_cast<uint8_t>(Pattern::Row), 1};
  EXPECT_EQ(decodeRequest(row.data(), row.size(), request), Error::None);
  EXPECT_EQ(request.command, Command::Pattern);
  EXPECT_EQ(request.pattern, Pattern::Row);
  EXPECT_EQ(request.variant, 1);

  constexpr std::array<uint8_t, 2> rawCommand{0xff, 0};
  EXPECT_EQ(decodeRequest(rawCommand.data(), 1, request), Error::UnknownCommand);
  EXPECT_EQ(decodeRequest(row.data(), row.size() - 1, request), Error::Malformed);
  constexpr std::array<uint8_t, 3> badVariant{static_cast<uint8_t>(Command::Pattern),
                                              static_cast<uint8_t>(Pattern::Cell), 2};
  EXPECT_EQ(decodeRequest(badVariant.data(), badVariant.size(), request), Error::InvalidArgument);
  constexpr std::array<uint8_t, 3> largestPattern{static_cast<uint8_t>(Command::Pattern),
                                                  static_cast<uint8_t>(Pattern::Burst100), 1};
  EXPECT_EQ(decodeRequest(largestPattern.data(), largestPattern.size(), request), Error::None);
  constexpr std::array<uint8_t, 3> unknownPattern{static_cast<uint8_t>(Command::Pattern),
                                                  static_cast<uint8_t>(Pattern::Burst100) + 1, 0};
  EXPECT_EQ(decodeRequest(unknownPattern.data(), unknownPattern.size(), request), Error::InvalidArgument);
  constexpr std::array<uint8_t, 1> metrics{static_cast<uint8_t>(Command::Metrics)};
  EXPECT_EQ(decodeRequest(metrics.data(), metrics.size(), request), Error::None);
  EXPECT_EQ(request.command, Command::Metrics);
}

TEST(TerminalDiagnosticsTest, LimitsInSessionControlsToSafeDisplayOperations) {
  using namespace knietty::diagnostics;
  Request request;

  request.command = Command::SessionInfo;
  EXPECT_TRUE(isTerminalControlAllowed(request));
  request.command = Command::SetPolarity;
  EXPECT_TRUE(isTerminalControlAllowed(request));
  request.command = Command::Clean;
  EXPECT_TRUE(isTerminalControlAllowed(request));
  request.command = Command::Metrics;
  EXPECT_TRUE(isTerminalControlAllowed(request));

  request.command = Command::Reset;
  EXPECT_FALSE(isTerminalControlAllowed(request));
  request.command = Command::Pattern;
  EXPECT_FALSE(isTerminalControlAllowed(request));
  request.command = Command::Stop;
  EXPECT_FALSE(isTerminalControlAllowed(request));
}

TEST(TerminalDiagnosticsTest, AppliesNamedPatternsDeterministically) {
  using namespace knietty::diagnostics;
  TerminalScreen screen;
  screen.takeDirtyRegion();
  Request request{Command::Pattern, Pattern::Cell, 1};
  applyRequest(screen, request);
  EXPECT_EQ(screen.getCell(2, 2).codepoint, ' ');
  EXPECT_EQ(screen.getCell(2, 2).attributes, TerminalScreen::ATTR_INVERSE);
  EXPECT_FALSE(screen.isCursorVisible());
  const auto cellDirty = screen.takeDirtyRegion();
  EXPECT_NE(cellDirty.rows & (uint32_t{1} << 2), 0u);

  request = {Command::Pattern, Pattern::DisjointRows, 1};
  applyRequest(screen, request);
  const auto rowsDirty = screen.takeDirtyRegion();
  EXPECT_NE(rowsDirty.rows & (uint32_t{1} << 4), 0u);
  EXPECT_NE(rowsDirty.rows & (uint32_t{1} << 18), 0u);

  request = {Command::Pattern, Pattern::BoundaryUnder, 0};
  applyRequest(screen, request);
  const auto boundaryUnder = screen.takeDirtyRegion();
  EXPECT_NE(boundaryUnder.rows & (uint32_t{1} << 9), 0u);
  EXPECT_NE(boundaryUnder.rows & (uint32_t{1} << 12), 0u);
  EXPECT_EQ(boundaryUnder.rows & (uint32_t{1} << 13), 0u);

  request = {Command::Pattern, Pattern::Burst100, 1};
  applyRequest(screen, request);
  EXPECT_EQ(screen.getCell(11, 1).attributes, TerminalScreen::ATTR_INVERSE);
  constexpr uint8_t overflowLastColumn = 100 - (TerminalScreen::COLS - 1) - 1;
  EXPECT_EQ(screen.getCell(12, overflowLastColumn).attributes, TerminalScreen::ATTR_INVERSE);
}

TEST(TerminalDiagnosticsTest, EncodesRefreshTelemetryInNetworkOrder) {
  using namespace knietty::diagnostics;
  RefreshEvent event;
  event.phase = EventPhase::Presented;
  event.command = Command::Pattern;
  event.timestampUs = 0x01020304;
  event.firstSequence = 0x11223344;
  event.lastSequence = 0x55667788;
  std::array<uint8_t, REFRESH_EVENT_PAYLOAD_SIZE> payload{};
  ASSERT_EQ(encodeRefreshEvent(payload.data(), payload.size(), event), payload.size());
  EXPECT_EQ(payload[0], SCHEMA_VERSION);
  EXPECT_EQ(payload[1], static_cast<uint8_t>(EventPhase::Presented));
  EXPECT_EQ(payload[8], 0x01);
  EXPECT_EQ(payload[9], 0x02);
  EXPECT_EQ(payload[10], 0x03);
  EXPECT_EQ(payload[11], 0x04);
  EXPECT_EQ(payload[92], 0x11);
  EXPECT_EQ(payload[95], 0x44);
  EXPECT_EQ(payload[96], 0x55);
  EXPECT_EQ(payload[99], 0x88);
}

TEST(TerminalDiagnosticsTest, EncodesReadOnlyMetricsInNetworkOrder) {
  using namespace knietty::diagnostics;
  MetricsSnapshot metrics;
  metrics.updates = 0x01020304;
  metrics.windowed = 5;
  metrics.fallback = 6;
  metrics.lastTotalUs = 0x11121314;
  metrics.lastRegionWidth = 800;
  metrics.lastRegionHeight = 432;
  metrics.lastRegionBytes = 42768;
  metrics.freeHeap = 53416;
  metrics.minimumFreeHeap = 44588;
  metrics.rxBytes = 0x21222324;
  metrics.rxReads = 0x25262728;
  metrics.burstEnds = 0x31323334;
  metrics.burstSnapshots = 0x35363738;
  metrics.burstTimeouts = 0x41424344;
  metrics.asyncTailUpdates = 0x45464748;
  std::array<uint8_t, METRICS_RESPONSE_PAYLOAD_SIZE> payload{};

  ASSERT_EQ(encodeMetricsResponse(payload.data(), payload.size(), metrics), payload.size());
  EXPECT_EQ(payload[0], SCHEMA_VERSION);
  EXPECT_EQ(payload[1], static_cast<uint8_t>(Command::Metrics));
  EXPECT_EQ(payload[2], static_cast<uint8_t>(Status::Accepted));
  EXPECT_EQ(payload[4], 0x01);
  EXPECT_EQ(payload[7], 0x04);
  EXPECT_EQ(payload[24], 0x11);
  EXPECT_EQ(payload[27], 0x14);
  EXPECT_EQ(payload[68], 0x03);
  EXPECT_EQ(payload[69], 0x20);
  EXPECT_EQ(payload[70], 0x01);
  EXPECT_EQ(payload[71], 0xb0);
  EXPECT_EQ(payload[84], 0x21);
  EXPECT_EQ(payload[87], 0x24);
  EXPECT_EQ(payload[92], 0x31);
  EXPECT_EQ(payload[95], 0x34);
  EXPECT_EQ(payload[104], 0x45);
  EXPECT_EQ(payload[107], 0x48);
}

}  // namespace
