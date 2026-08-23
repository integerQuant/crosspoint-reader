#pragma once

#include <cstddef>
#include <cstdint>

#include "TerminalScreen.h"

class TerminalParser {
 public:
  struct ReplySink {
    void* context = nullptr;
    size_t (*write)(void* context, const uint8_t* data, size_t length) = nullptr;
  };

  explicit TerminalParser(TerminalScreen& screen) : screen(screen) {}
  TerminalParser(TerminalScreen& screen, const ReplySink replySink) : screen(screen), replySink(replySink) {}

  void reset();
  void feed(uint8_t byte);
  void setReplySink(const ReplySink sink) { replySink = sink; }

  bool isPresentationHeld() const { return presentationHeld; }
  bool takePresentationBoundary();
  void releasePresentationHold();
  bool isAlternateScreen() const { return alternateScreen; }

 private:
  enum class State : uint8_t {
    Ground,
    Escape,
    EscapeCharset,
    Csi,
    ControlString,
    ControlStringEscape,
  };

  enum class ControlStringKind : uint8_t {
    None,
    Osc,
    Dcs,
    Other,
  };

  static constexpr uint8_t MAX_PARAMS = 16;
  static constexpr uint8_t CONTROL_STRING_CAPACITY = 32;

  TerminalScreen& screen;
  ReplySink replySink;
  State state = State::Ground;
  uint16_t params[MAX_PARAMS]{};
  uint8_t paramSeparators[MAX_PARAMS]{};
  uint8_t paramCount = 0;
  bool paramPresent = false;
  uint8_t nextParamSeparator = 0;
  uint8_t privateMarker = 0;
  uint8_t intermediate = 0;
  ControlStringKind controlStringKind = ControlStringKind::None;
  char controlString[CONTROL_STRING_CAPACITY + 1]{};
  uint8_t controlStringLength = 0;
  bool controlStringOverflow = false;
  uint8_t scrollTop = 0;
  uint8_t scrollBottom = TerminalScreen::ROWS - 1;
  uint8_t savedCursorRow = 0;
  uint8_t savedCursorColumn = 0;
  uint8_t savedAttributes = TerminalScreen::ATTR_NONE;
  bool savedCursorValid = false;
  uint32_t lastGraphicCodepoint = 0;
  uint8_t lastGraphicAttributes = TerminalScreen::ATTR_NONE;
  bool lastGraphicValid = false;
  bool presentationHeld = false;
  bool presentationBoundary = false;
  bool alternateScreen = false;
  uint32_t utf8Codepoint = 0;
  uint32_t utf8Minimum = 0;
  uint8_t utf8Remaining = 0;

  void beginCsi();
  void beginControlString(ControlStringKind kind);
  void resetUtf8();
  void resetScreenModes();
  void feedGround(uint8_t byte);
  void feedEscape(uint8_t byte);
  void feedControlString(uint8_t byte);
  void finishControlString();
  void finishParam();
  uint16_t parameter(uint8_t index, uint16_t defaultValue, bool zeroMeansDefault = false) const;
  void dispatch(uint8_t finalByte);
  void dispatchPrivate(uint8_t finalByte);
  void dispatchSgr();
  void dispatchControlString();
  void putGraphic(uint32_t codepoint);
  void repeatLastGraphic(uint16_t count);
  void setAlternateScreen(bool enabled);
  void reply(const char* response, size_t length) const;
  void replyCursorPosition() const;
  void replyModeStatus(uint16_t mode) const;
};

// Keep control-sequence support tiny relative to the two 7.5 KiB screen
// snapshots. This is 152 bytes on the 64-bit native test target and smaller on
// the ESP32-C3; additions must remain fixed-size and justify raising the gate.
static_assert(sizeof(TerminalParser) <= 160);
