#include "TerminalParser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {
bool isZeroWidthFormat(const uint32_t codepoint) {
  return codepoint == 0x200b || codepoint == 0x200c || codepoint == 0x200d || codepoint == 0xfe0e ||
         codepoint == 0xfe0f;
}
}  // namespace

void TerminalParser::reset() {
  state = State::Ground;
  beginCsi();
  resetUtf8();
  controlStringKind = ControlStringKind::None;
  controlStringLength = 0;
  controlStringOverflow = false;
  resetScreenModes();
  presentationHeld = false;
  presentationBoundary = false;
  alternateScreen = false;
}

bool TerminalParser::takePresentationBoundary() {
  const bool boundary = presentationBoundary;
  presentationBoundary = false;
  return boundary;
}

void TerminalParser::releasePresentationHold() {
  if (!presentationHeld) return;
  presentationHeld = false;
  presentationBoundary = true;
}

void TerminalParser::beginCsi() {
  std::fill(std::begin(params), std::end(params), 0);
  std::fill(std::begin(paramSeparators), std::end(paramSeparators), 0);
  paramCount = 0;
  paramPresent = false;
  nextParamSeparator = 0;
  privateMarker = 0;
  intermediate = 0;
}

void TerminalParser::beginControlString(const ControlStringKind kind) {
  controlStringKind = kind;
  controlStringLength = 0;
  controlStringOverflow = false;
  controlString[0] = '\0';
  state = State::ControlString;
}

void TerminalParser::resetUtf8() {
  utf8Codepoint = 0;
  utf8Minimum = 0;
  utf8Remaining = 0;
}

void TerminalParser::resetScreenModes() {
  scrollTop = 0;
  scrollBottom = TerminalScreen::ROWS - 1;
  savedCursorRow = 0;
  savedCursorColumn = 0;
  savedAttributes = TerminalScreen::ATTR_NONE;
  savedCursorValid = false;
  lastGraphicCodepoint = 0;
  lastGraphicAttributes = TerminalScreen::ATTR_NONE;
  lastGraphicValid = false;
}

void TerminalParser::putGraphic(const uint32_t codepoint) {
  if (isZeroWidthFormat(codepoint)) return;
  screen.putCodepoint(codepoint, scrollTop, scrollBottom);
  lastGraphicCodepoint = codepoint;
  lastGraphicAttributes = screen.getAttributes();
  lastGraphicValid = true;
}

void TerminalParser::repeatLastGraphic(const uint16_t count) {
  if (!lastGraphicValid) return;
  const uint8_t attributes = screen.getAttributes();
  screen.setAttributes(lastGraphicAttributes);
  const uint16_t boundedCount = std::min<uint16_t>(count, TerminalScreen::COLS * TerminalScreen::ROWS);
  for (uint16_t index = 0; index < boundedCount; ++index) {
    screen.putCodepoint(lastGraphicCodepoint, scrollTop, scrollBottom);
  }
  screen.setAttributes(attributes);
}

void TerminalParser::feedGround(const uint8_t byte) {
  if (utf8Remaining > 0) {
    if ((byte & 0xc0) != 0x80) {
      resetUtf8();
      putGraphic(TerminalScreen::REPLACEMENT_CODEPOINT);
      feed(byte);
      return;
    }
    utf8Codepoint = (utf8Codepoint << 6) | (byte & 0x3f);
    --utf8Remaining;
    if (utf8Remaining == 0) {
      const uint32_t codepoint = utf8Codepoint;
      const bool valid =
          codepoint >= utf8Minimum && codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      resetUtf8();
      putGraphic(valid ? codepoint : TerminalScreen::REPLACEMENT_CODEPOINT);
    }
    return;
  }

  if (byte < 0x80) {
    switch (byte) {
      case 0x1b:
        state = State::Escape;
        return;
      case '\n':
        screen.lineFeed(scrollTop, scrollBottom);
        return;
      case '\r':
        screen.carriageReturn();
        return;
      case '\b':
        screen.backspace();
        return;
      case '\t':
        screen.tab();
        return;
      case 0x07:
        return;
      default:
        if (byte >= 0x20 && byte != 0x7f) putGraphic(byte);
        return;
    }
  }

  if (byte >= 0xc2 && byte <= 0xdf) {
    utf8Codepoint = byte & 0x1f;
    utf8Minimum = 0x80;
    utf8Remaining = 1;
  } else if (byte >= 0xe0 && byte <= 0xef) {
    utf8Codepoint = byte & 0x0f;
    utf8Minimum = 0x800;
    utf8Remaining = 2;
  } else if (byte >= 0xf0 && byte <= 0xf4) {
    utf8Codepoint = byte & 0x07;
    utf8Minimum = 0x10000;
    utf8Remaining = 3;
  } else {
    putGraphic(TerminalScreen::REPLACEMENT_CODEPOINT);
  }
}

void TerminalParser::feedEscape(const uint8_t byte) {
  state = State::Ground;
  switch (byte) {
    case '[':
      beginCsi();
      state = State::Csi;
      break;
    case ']':
      beginControlString(ControlStringKind::Osc);
      break;
    case 'P':
      beginControlString(ControlStringKind::Dcs);
      break;
    case 'X':
    case '^':
    case '_':
      beginControlString(ControlStringKind::Other);
      break;
    case '(':
    case ')':
    case '*':
    case '+':
      state = State::EscapeCharset;
      break;
    case '7':
      savedCursorRow = screen.getCursorRow();
      savedCursorColumn = screen.getCursorColumn();
      savedAttributes = screen.getAttributes();
      savedCursorValid = true;
      break;
    case '8':
      if (savedCursorValid) {
        screen.setAttributes(savedAttributes);
        screen.setCursor(savedCursorRow + 1, savedCursorColumn + 1);
      }
      break;
    case 'D':
      screen.lineFeed(scrollTop, scrollBottom);
      break;
    case 'E':
      screen.lineFeed(scrollTop, scrollBottom);
      screen.carriageReturn();
      break;
    case 'M':
      screen.reverseIndex(scrollTop, scrollBottom);
      break;
    case 'c':
      screen.reset();
      reset();
      break;
    case 0x1b:
      state = State::Escape;
      break;
    default:
      break;
  }
}

void TerminalParser::feedControlString(const uint8_t byte) {
  if (state == State::ControlStringEscape) {
    if (byte == '\\') {
      finishControlString();
    } else if (byte != 0x1b) {
      state = State::ControlString;
    }
    return;
  }

  if (controlStringKind == ControlStringKind::Osc && byte == 0x07) {
    finishControlString();
  } else if (byte == 0x18 || byte == 0x1a) {
    controlStringKind = ControlStringKind::None;
    state = State::Ground;
  } else if (byte == 0x1b) {
    state = State::ControlStringEscape;
  } else if (controlStringLength < CONTROL_STRING_CAPACITY) {
    controlString[controlStringLength++] = static_cast<char>(byte);
    controlString[controlStringLength] = '\0';
  } else {
    controlStringOverflow = true;
  }
}

void TerminalParser::finishControlString() {
  if (!controlStringOverflow) dispatchControlString();
  controlStringKind = ControlStringKind::None;
  controlStringLength = 0;
  controlStringOverflow = false;
  state = State::Ground;
}

void TerminalParser::finishParam() {
  if (paramCount < MAX_PARAMS) ++paramCount;
  paramPresent = false;
}

uint16_t TerminalParser::parameter(const uint8_t index, const uint16_t defaultValue,
                                   const bool zeroMeansDefault) const {
  if (index >= paramCount) return defaultValue;
  const uint16_t value = params[index];
  return zeroMeansDefault && value == 0 ? defaultValue : value;
}

void TerminalParser::feed(const uint8_t byte) {
  if (state == State::Ground) return feedGround(byte);
  if (state == State::Escape) return feedEscape(byte);
  if (state == State::EscapeCharset) {
    state = byte == 0x1b ? State::Escape : State::Ground;
    return;
  }
  if (state == State::ControlString || state == State::ControlStringEscape) return feedControlString(byte);

  if (byte == 0x1b) {
    state = State::Escape;
    return;
  }
  if (byte == 0x18 || byte == 0x1a) {
    state = State::Ground;
    return;
  }

  if (byte >= '0' && byte <= '9' && intermediate == 0) {
    if (!paramPresent && paramCount < MAX_PARAMS) paramSeparators[paramCount] = nextParamSeparator;
    paramPresent = true;
    if (paramCount < MAX_PARAMS) {
      params[paramCount] = std::min<uint16_t>(9999, params[paramCount] * 10 + (byte - '0'));
    }
    return;
  }
  if ((byte == ';' || byte == ':') && intermediate == 0) {
    if (!paramPresent && paramCount < MAX_PARAMS) paramSeparators[paramCount] = nextParamSeparator;
    finishParam();
    nextParamSeparator = byte;
    return;
  }
  if (byte >= 0x3c && byte <= 0x3f && paramCount == 0 && !paramPresent && privateMarker == 0 && intermediate == 0) {
    privateMarker = byte;
    return;
  }
  if (byte >= 0x20 && byte <= 0x2f) {
    intermediate = intermediate == 0 ? byte : 0xff;
    return;
  }
  if (byte >= 0x40 && byte <= 0x7e) {
    if (paramPresent || paramCount > 0) finishParam();
    dispatch(byte);
    state = State::Ground;
    return;
  }

  // Consume unsupported parameter bytes until the final byte so their tail
  // cannot leak into the terminal as printable text.
  if (byte < 0x30 || byte > 0x3f) state = State::Ground;
}

void TerminalParser::dispatch(const uint8_t finalByte) {
  if (privateMarker != 0) return dispatchPrivate(finalByte);

  // Cursor-style selection (CSI Ps SP q) and other unsupported intermediate
  // forms are intentionally consumed as complete sequences.
  if (intermediate != 0) return;

  switch (finalByte) {
    case '@':
      screen.insertCharacters(parameter(0, 1, true));
      break;
    case 'A':
      screen.moveCursor(-static_cast<int>(parameter(0, 1, true)), 0);
      break;
    case 'B':
      screen.moveCursor(static_cast<int>(parameter(0, 1, true)), 0);
      break;
    case 'C':
      screen.moveCursor(0, static_cast<int>(parameter(0, 1, true)));
      break;
    case 'D':
      screen.moveCursor(0, -static_cast<int>(parameter(0, 1, true)));
      break;
    case 'E':
      screen.moveCursor(static_cast<int>(parameter(0, 1, true)), 0);
      screen.setCursorColumn(1);
      break;
    case 'F':
      screen.moveCursor(-static_cast<int>(parameter(0, 1, true)), 0);
      screen.setCursorColumn(1);
      break;
    case 'G':
      screen.setCursorColumn(parameter(0, 1, true));
      break;
    case 'H':
    case 'f':
      screen.setCursor(parameter(0, 1, true), parameter(1, 1, true));
      break;
    case 'J':
      screen.clearDisplay(parameter(0, 0));
      break;
    case 'K':
      screen.clearLine(parameter(0, 0));
      break;
    case 'L':
      screen.insertLines(scrollTop, scrollBottom, parameter(0, 1, true));
      break;
    case 'M':
      screen.deleteLines(scrollTop, scrollBottom, parameter(0, 1, true));
      break;
    case 'P':
      screen.deleteCharacters(parameter(0, 1, true));
      break;
    case 'S':
      screen.scrollUp(scrollTop, scrollBottom,
                      static_cast<uint8_t>(std::min<uint16_t>(parameter(0, 1, true), TerminalScreen::ROWS)));
      break;
    case 'T':
      screen.scrollDown(scrollTop, scrollBottom,
                        static_cast<uint8_t>(std::min<uint16_t>(parameter(0, 1, true), TerminalScreen::ROWS)));
      break;
    case 'X':
      screen.eraseCharacters(parameter(0, 1, true));
      break;
    case 'b':
      repeatLastGraphic(parameter(0, 1, true));
      break;
    case 'c':
      if (parameter(0, 0) == 0) reply("\x1b[?1;0c", 7);
      break;
    case 'd':
      screen.setCursorRow(parameter(0, 1, true));
      break;
    case 'm':
      dispatchSgr();
      break;
    case 'n':
      if (parameter(0, 0) == 5) {
        reply("\x1b[0n", 4);
      } else if (parameter(0, 0) == 6) {
        replyCursorPosition();
      }
      break;
    case 'r': {
      const uint16_t top = parameter(0, 1, true);
      const uint16_t bottom = parameter(1, TerminalScreen::ROWS, true);
      if (top < bottom && bottom <= TerminalScreen::ROWS) {
        scrollTop = static_cast<uint8_t>(top - 1);
        scrollBottom = static_cast<uint8_t>(bottom - 1);
        screen.setCursor(1, 1);
      }
      break;
    }
    case 's':
      savedCursorRow = screen.getCursorRow();
      savedCursorColumn = screen.getCursorColumn();
      savedAttributes = screen.getAttributes();
      savedCursorValid = true;
      break;
    case 'u':
      if (savedCursorValid) {
        screen.setAttributes(savedAttributes);
        screen.setCursor(savedCursorRow + 1, savedCursorColumn + 1);
      }
      break;
    default:
      break;
  }
}

void TerminalParser::dispatchPrivate(const uint8_t finalByte) {
  if (privateMarker == '>' && intermediate == 0 && finalByte == 'q') {
    constexpr char RESPONSE[] = "\x1bP>|knietty 0.1.3\x1b\\";
    reply(RESPONSE, sizeof(RESPONSE) - 1);
    return;
  }

  if (privateMarker != '?') return;
  if (intermediate == '$' && finalByte == 'p') {
    replyModeStatus(parameter(0, 0));
    return;
  }
  if (intermediate != 0 || (finalByte != 'h' && finalByte != 'l')) return;

  const bool enabled = finalByte == 'h';
  for (uint8_t index = 0; index < paramCount; ++index) {
    switch (params[index]) {
      case 25:
        screen.setCursorVisible(enabled);
        break;
      case 47:
      case 1047:
      case 1049:
        setAlternateScreen(enabled);
        break;
      case 2026:
        if (enabled) {
          presentationHeld = true;
        } else if (presentationHeld) {
          presentationHeld = false;
          presentationBoundary = true;
        }
        break;
      default:
        break;
    }
  }
}

void TerminalParser::dispatchSgr() {
  uint8_t attributes = screen.getAttributes();
  if (paramCount == 0) {
    screen.setAttributes(TerminalScreen::ATTR_NONE);
    return;
  }

  for (uint8_t index = 0; index < paramCount; ++index) {
    const uint16_t value = params[index];
    if (value == 38 || value == 48 || value == 58) {
      if (index + 1 >= paramCount) continue;
      ++index;
      const uint16_t colorMode = params[index];
      if (paramSeparators[index] == ':') {
        while (index + 1 < paramCount && paramSeparators[index + 1] == ':') ++index;
      } else if (colorMode == 2) {
        index = static_cast<uint8_t>(std::min<uint16_t>(paramCount - 1, index + 3));
      } else if (colorMode == 5) {
        index = static_cast<uint8_t>(std::min<uint16_t>(paramCount - 1, index + 1));
      }
      continue;
    }

    switch (value) {
      case 0:
        attributes = TerminalScreen::ATTR_NONE;
        break;
      case 1:
        attributes |= TerminalScreen::ATTR_BOLD;
        break;
      case 4:
      case 21:
        attributes |= TerminalScreen::ATTR_UNDERLINE;
        break;
      case 7:
        attributes |= TerminalScreen::ATTR_INVERSE;
        break;
      case 8:
        attributes |= TerminalScreen::ATTR_HIDDEN;
        break;
      case 9:
        attributes |= TerminalScreen::ATTR_STRIKETHROUGH;
        break;
      case 22:
        attributes &= ~TerminalScreen::ATTR_BOLD;
        break;
      case 24:
        attributes &= ~TerminalScreen::ATTR_UNDERLINE;
        break;
      case 27:
        attributes &= ~TerminalScreen::ATTR_INVERSE;
        break;
      case 28:
        attributes &= ~TerminalScreen::ATTR_HIDDEN;
        break;
      case 29:
        attributes &= ~TerminalScreen::ATTR_STRIKETHROUGH;
        break;
      default:
        break;
    }
  }
  screen.setAttributes(attributes);
}

void TerminalParser::dispatchControlString() {
  if (controlStringKind == ControlStringKind::Osc) {
    if (std::strcmp(controlString, "10;?") == 0) {
      constexpr char RESPONSE[] = "\x1b]10;rgb:0000/0000/0000\x1b\\";
      reply(RESPONSE, sizeof(RESPONSE) - 1);
    } else if (std::strcmp(controlString, "11;?") == 0) {
      constexpr char RESPONSE[] = "\x1b]11;rgb:ffff/ffff/ffff\x1b\\";
      reply(RESPONSE, sizeof(RESPONSE) - 1);
    }
  } else if (controlStringKind == ControlStringKind::Dcs && std::strcmp(controlString, "+q4d73") == 0) {
    constexpr char RESPONSE[] = "\x1bP0+r4d73\x1b\\";
    reply(RESPONSE, sizeof(RESPONSE) - 1);
  }
}

void TerminalParser::setAlternateScreen(const bool enabled) {
  if (alternateScreen == enabled) return;
  alternateScreen = enabled;
  screen.reset();
  resetScreenModes();
}

void TerminalParser::reply(const char* const response, const size_t length) const {
  if (replySink.write == nullptr || length == 0) return;
  replySink.write(replySink.context, reinterpret_cast<const uint8_t*>(response), length);
}

void TerminalParser::replyCursorPosition() const {
  char response[24]{};
  const int length =
      std::snprintf(response, sizeof(response), "\x1b[%u;%uR", static_cast<unsigned>(screen.getCursorRow() + 1),
                    static_cast<unsigned>(screen.getCursorColumn() + 1));
  if (length > 0) reply(response, static_cast<size_t>(length));
}

void TerminalParser::replyModeStatus(const uint16_t mode) const {
  const uint8_t status = mode == 2026 ? (presentationHeld ? 1 : 2) : 0;
  char response[24]{};
  const int length = std::snprintf(response, sizeof(response), "\x1b[?%u;%u$y", static_cast<unsigned>(mode),
                                   static_cast<unsigned>(status));
  if (length > 0) reply(response, static_cast<size_t>(length));
}
