#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"
#include "terminal/TerminalInput.h"
#include "terminal/TerminalParser.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalUsb.h"

class TerminalActivity final : public Activity {
 public:
  explicit TerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Terminal", renderer, mappedInput), parser(screen), terminalInput(mappedInput, usb) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  static constexpr size_t RX_RING_SIZE = 4096;
  static constexpr size_t RX_RING_MASK = RX_RING_SIZE - 1;
  static constexpr uint32_t INTERACTIVE_BATCH_MS = 100;
  static constexpr uint32_t MAX_BATCH_MS = 200;
  static constexpr uint32_t IDLE_CLEAN_MS = 3000;
  static constexpr uint16_t FAST_REFRESH_LIMIT = 40;
  static constexpr int HEADER_HEIGHT = 32;

  static_assert((RX_RING_SIZE & RX_RING_MASK) == 0, "RX ring size must be a power of two");

  TerminalScreen screen;
  TerminalParser parser;
  TerminalUsb usb;
  TerminalInput terminalInput;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Portrait;

  uint8_t rxRing[RX_RING_SIZE]{};
  std::atomic<size_t> rxHead{0};
  std::atomic<size_t> rxTail{0};
  std::atomic<bool> rxOverflow{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> statusDirty{true};
  std::atomic<bool> renderScheduled{false};
  std::atomic<bool> forceFullRefresh{true};
  std::atomic<uint32_t> firstQueuedAt{0};
  std::atomic<uint32_t> lastQueuedAt{0};
  std::atomic<uint32_t> lastTrafficAt{0};
  std::atomic<uint32_t> lastRenderAt{0};
  std::atomic<uint16_t> fastRefreshCount{0};
  bool firstRender = true;

  bool enqueue(uint8_t byte);
  bool hasQueuedBytes() const;
  void pollUsb(uint32_t now);
  void drainQueuedBytes();
  void scheduleRender(bool forceFull);
  void drawStatus();
  void drawDirtyRows(uint32_t dirtyRows);
};
