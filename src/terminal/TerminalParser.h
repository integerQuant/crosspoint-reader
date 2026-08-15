#pragma once

#include <cstdint>

#include "TerminalScreen.h"

class TerminalParser {
 public:
  explicit TerminalParser(TerminalScreen& screen) : screen(screen) {}

  void reset();
  void feed(uint8_t byte);

 private:
  enum class State : uint8_t { Ground, Escape, Csi };
  static constexpr uint8_t MAX_PARAMS = 8;

  TerminalScreen& screen;
  State state = State::Ground;
  uint16_t params[MAX_PARAMS]{};
  uint8_t paramCount = 0;
  bool paramPresent = false;
  bool privateMarker = false;

  void beginCsi();
  void finishParam();
  uint16_t parameter(uint8_t index, uint16_t defaultValue, bool zeroMeansDefault = false) const;
  void dispatch(uint8_t finalByte);
  void dispatchSgr();
};
