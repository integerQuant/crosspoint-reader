#include "TerminalParser.h"

#include <algorithm>
#include <iterator>

void TerminalParser::reset() {
  state = State::Ground;
  beginCsi();
  resetUtf8();
}

void TerminalParser::beginCsi() {
  std::fill(std::begin(params), std::end(params), 0);
  paramCount = 0;
  paramPresent = false;
  privateMarker = false;
}

void TerminalParser::resetUtf8() {
  utf8Codepoint = 0;
  utf8Minimum = 0;
  utf8Remaining = 0;
}

void TerminalParser::feedGround(const uint8_t byte) {
  if (utf8Remaining > 0) {
    if ((byte & 0xc0) != 0x80) {
      resetUtf8();
      screen.putCodepoint(TerminalScreen::REPLACEMENT_CODEPOINT);
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
      screen.putCodepoint(valid ? codepoint : TerminalScreen::REPLACEMENT_CODEPOINT);
    }
    return;
  }

  if (byte < 0x80) {
    switch (byte) {
      case 0x1b:
        state = State::Escape;
        return;
      case '\n':
        screen.lineFeed();
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
        if (byte >= 0x20) screen.putCodepoint(byte);
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
    screen.putCodepoint(TerminalScreen::REPLACEMENT_CODEPOINT);
  }
}

void TerminalParser::finishParam() {
  if (paramCount < MAX_PARAMS) {
    ++paramCount;
  }
  paramPresent = false;
}

uint16_t TerminalParser::parameter(const uint8_t index, const uint16_t defaultValue,
                                   const bool zeroMeansDefault) const {
  if (index >= paramCount) {
    return defaultValue;
  }
  const uint16_t value = params[index];
  return zeroMeansDefault && value == 0 ? defaultValue : value;
}

void TerminalParser::feed(const uint8_t byte) {
  if (state == State::Ground) {
    feedGround(byte);
    return;
  }

  if (state == State::Escape) {
    if (byte == '[') {
      beginCsi();
      state = State::Csi;
    } else {
      state = State::Ground;
    }
    return;
  }

  if (byte == 0x1b) {
    state = State::Escape;
    return;
  }

  if (byte >= '0' && byte <= '9') {
    paramPresent = true;
    if (paramCount < MAX_PARAMS) {
      params[paramCount] = std::min<uint16_t>(999, params[paramCount] * 10 + (byte - '0'));
    }
    return;
  }
  if (byte == ';') {
    finishParam();
    return;
  }
  if (byte == '?' && paramCount == 0 && !paramPresent) {
    privateMarker = true;
    return;
  }
  if (byte >= 0x40 && byte <= 0x7e) {
    if (paramPresent || paramCount > 0) {
      finishParam();
    }
    dispatch(byte);
  }
  state = State::Ground;
}

void TerminalParser::dispatch(const uint8_t finalByte) {
  if (privateMarker) {
    if (parameter(0, 0) == 25 && (finalByte == 'h' || finalByte == 'l')) {
      screen.setCursorVisible(finalByte == 'h');
    }
    return;
  }

  switch (finalByte) {
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
    case 'm':
      dispatchSgr();
      break;
    default:
      break;
  }
}

void TerminalParser::dispatchSgr() {
  uint8_t attributes = screen.getAttributes();
  if (paramCount == 0) {
    screen.setAttributes(TerminalScreen::ATTR_NONE);
    return;
  }

  for (uint8_t index = 0; index < paramCount; ++index) {
    switch (params[index]) {
      case 0:
        attributes = TerminalScreen::ATTR_NONE;
        break;
      case 1:
        attributes |= TerminalScreen::ATTR_BOLD;
        break;
      case 4:
        attributes |= TerminalScreen::ATTR_UNDERLINE;
        break;
      case 7:
        attributes |= TerminalScreen::ATTR_INVERSE;
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
      default:
        break;
    }
  }
  screen.setAttributes(attributes);
}
