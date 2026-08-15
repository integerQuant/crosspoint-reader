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
  static constexpr int HEADER_HEIGHT = 32;

  struct RefreshMetrics {
    uint32_t count = 0;
    uint32_t lastTotalUs = 0;
    uint32_t lastRenderUs = 0;
    uint32_t lastTransferUs = 0;
    uint32_t lastWaveformUs = 0;
    uint32_t minTotalUs = UINT32_MAX;
    uint32_t maxTotalUs = 0;
    uint64_t totalUs = 0;
    uint32_t windowedCount = 0;
    uint32_t fallbackCount = 0;
    uint32_t cleanCount = 0;

    void record(uint32_t total, uint32_t render, const HalDisplay::RefreshTiming& displayTiming, bool windowed,
                bool clean);
  };

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
  char displayHostname[32]{};
  char displayLocalIp[16]{};
  TerminalWifi::State renderDisplayState = TerminalWifi::State::Offline;
  char renderClientName[33]{};
  char renderClientIp[16]{};
  char renderClock[6]{};
  uint16_t renderBattery = 0;
  char renderHostname[32]{};
  char renderLocalIp[16]{};
  RefreshMetrics refreshMetrics;
  RefreshMetrics renderRefreshMetrics;

  std::atomic<bool> contentDirty{false};
  std::atomic<bool> statusDirty{true};
  std::atomic<bool> clearContentArea{false};
  std::atomic<bool> exitConfirmationArmed{false};
  std::atomic<bool> waitingDiagnostics{false};
  std::atomic<bool> renderScheduled{false};
  std::atomic<bool> forceFullRefresh{true};
  std::atomic<uint32_t> firstQueuedAt{0};
  std::atomic<uint32_t> lastQueuedAt{0};
  uint32_t lastNetworkGeneration = 0;
  uint32_t exitConfirmUntil = 0;
  bool terminalStarted = false;
  bool firstRender = true;
  bool renderExitConfirmation = false;
  bool terminalInverted = false;
  bool renderInverted = false;
  bool framebufferInverted = false;
  bool renderWaitingDiagnostics = false;

  void startTerminal();
  void pollWifi(uint32_t now);
  void syncNetworkState();
  void syncClock();
  bool handlePowerButton(uint32_t now);
  void toggleInversion();
  void scheduleRender(bool forceFull);
  void drawStatus();
  void drawWaitingScreen();
  void drawRefreshDiagnostics();
  void drawApprovalPrompt();
  void drawContextualHints(const MappedInputManager::Labels& labels);
  void drawDirtyCells(const TerminalScreen::DirtyRegion& dirtyRegion);
};
