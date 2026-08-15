#pragma once

class MappedInputManager;
class TerminalWifi;

class TerminalInput {
 public:
  TerminalInput(MappedInputManager& input, TerminalWifi& wifi) : input(input), wifi(wifi) {}

  // Returns true when the long-Back exit gesture completed.
  bool poll();

 private:
  static constexpr unsigned long LONG_PRESS_MS = 1000;

  MappedInputManager& input;
  TerminalWifi& wifi;

  void send(const char* sequence, unsigned int length) const;
};
