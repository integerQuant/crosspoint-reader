#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "activities/Activity.h"
#include "terminal/TerminalInput.h"
#include "terminal/TerminalParser.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWifi.h"

class TerminalActivity final : public Activity {
 public:
  explicit TerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Terminal", renderer, mappedInput), parser(screen), terminalInput(mappedInput, wifi) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool ownsPowerButton() const override { return terminalStarted; }
  bool skipLoopDelay() override { return terminalStarted; }

 private:
  static constexpr uint32_t INTERACTIVE_BATCH_MS = 8;
  static constexpr uint32_t MAX_BATCH_MS = 20;
  static constexpr uint32_t EXIT_CONFIRM_MS = 3000;
  static constexpr uint16_t FAST_REFRESH_LIMIT = 50;
  static constexpr int HEADER_HEIGHT = 32;
  static constexpr int TERMINAL_TOP = 40;
  static constexpr int TERMINAL_LEFT = 40;

  mutable std::mutex modelMutex;
  TerminalScreen screen;
  TerminalScreen renderScreen;
  TerminalParser parser;
  TerminalWifi wifi;
  TerminalInput terminalInput;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Portrait;
  TerminalWifi::State displayState = TerminalWifi::State::Offline;
  char displayClientName[33]{};
  char displayClientIp[16]{};
  char displayClock[6]{};
  uint16_t displayBattery = 101;
  TerminalWifi::State renderDisplayState = TerminalWifi::State::Offline;
  char renderClientName[33]{};
  char renderClientIp[16]{};
  char renderClock[6]{};
  uint16_t renderBattery = 0;

  std::atomic<bool> contentDirty{false};
  std::atomic<bool> statusDirty{true};
  std::atomic<bool> exitConfirmationArmed{false};
  std::atomic<bool> renderScheduled{false};
  std::atomic<bool> forceFullRefresh{true};
  std::atomic<uint32_t> firstQueuedAt{0};
  std::atomic<uint32_t> lastQueuedAt{0};
  std::atomic<uint16_t> fastRefreshCount{0};
  uint32_t lastNetworkGeneration = 0;
  uint32_t exitConfirmUntil = 0;
  bool terminalStarted = false;
  bool firstRender = true;
  bool renderExitConfirmation = false;
  bool terminalInverted = false;
  bool renderInverted = false;
  bool framebufferInverted = false;

  void startTerminal();
  void pollWifi(uint32_t now);
  void syncNetworkState();
  void syncClock();
  bool handlePowerButton(uint32_t now);
  void toggleInversion();
  void scheduleRender(bool forceFull);
  void drawStatus();
  void drawApprovalPrompt();
  void drawDirtyRows(uint32_t dirtyRows);
};
