#pragma once

class MappedInputManager;
class TerminalUsb;

class TerminalInput {
 public:
  TerminalInput(MappedInputManager& input, TerminalUsb& usb) : input(input), usb(usb) {}

  // Returns true when the long-Back exit gesture completed.
  bool poll();

 private:
  static constexpr unsigned long LONG_PRESS_MS = 1000;

  MappedInputManager& input;
  TerminalUsb& usb;

  void send(const char* sequence, unsigned int length) const;
};
