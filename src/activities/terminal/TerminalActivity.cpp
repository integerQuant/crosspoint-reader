#include "TerminalActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"
#include "terminal/TerminalFont.h"

namespace {

constexpr char RX_OVERFLOW_NOTICE[] = "\r\n[knietty: RX overflow]\r\n";

}  // namespace

void TerminalActivity::onEnter() {
  Activity::onEnter();
  previousOrientation = renderer.getOrientation();

  auto selection = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!selection) {
    LOG_ERR("KNIETTY", "OOM: WiFi selection activity");
    finish();
    return;
  }
  startActivityForResult(std::move(selection), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    startTerminal();
  });
}

void TerminalActivity::startTerminal() {
  wifi.begin();

  {
    RenderLock lock;
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    screen.reset();
    parser.reset();
    terminalStarted = true;
  }

  rxHead.store(0);
  rxTail.store(0);
  rxOverflow.store(false);
  connected.store(wifi.isConnected());
  displayState = wifi.getState();
  displayClientName[0] = '\0';
  displayClientIp[0] = '\0';
  lastNetworkGeneration = wifi.getGeneration();
  statusDirty.store(true);
  forceFullRefresh.store(true);
  firstQueuedAt.store(0);
  lastQueuedAt.store(0);
  lastTrafficAt.store(millis());
  lastRenderAt.store(0);
  fastRefreshCount.store(0);
  firstRender = true;
  renderScheduled.store(true);
  requestUpdate();
}

void TerminalActivity::onExit() {
  wifi.end();
  // ActivityManager calls onExit while holding RenderLock, so restoring the
  // shared orientation here is synchronized with any in-flight render.
  if (terminalStarted) renderer.setOrientation(previousOrientation);
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool TerminalActivity::enqueue(const uint8_t byte) {
  const size_t head = rxHead.load(std::memory_order_relaxed);
  const size_t next = (head + 1) & RX_RING_MASK;
  if (next == rxTail.load(std::memory_order_acquire)) {
    rxOverflow.store(true, std::memory_order_release);
    return false;
  }
  rxRing[head] = byte;
  rxHead.store(next, std::memory_order_release);
  return true;
}

bool TerminalActivity::hasQueuedBytes() const {
  return rxHead.load(std::memory_order_acquire) != rxTail.load(std::memory_order_acquire);
}

void TerminalActivity::pollWifi(const uint32_t now) {
  bool received = false;
  // Keep each main-loop slice short. The 4 KiB SPSC ring absorbs bytes while
  // the render task waits for an e-ink waveform to finish.
  for (uint16_t count = 0; count < 256 && wifi.available() > 0; ++count) {
    const int byte = wifi.read();
    if (byte < 0) {
      break;
    }
    enqueue(static_cast<uint8_t>(byte));
    received = true;
  }

  if (received) {
    lastTrafficAt.store(now, std::memory_order_relaxed);
    lastQueuedAt.store(now, std::memory_order_relaxed);
    uint32_t expected = 0;
    firstQueuedAt.compare_exchange_strong(expected, now, std::memory_order_relaxed);
  }
}

void TerminalActivity::syncNetworkState() {
  const uint32_t generation = wifi.getGeneration();
  if (generation == lastNetworkGeneration) return;

  {
    RenderLock lock(*this);
    const TerminalWifi::State previousState = displayState;
    displayState = wifi.getState();
    std::snprintf(displayClientName, sizeof(displayClientName), "%s", wifi.getClientName());
    std::snprintf(displayClientIp, sizeof(displayClientIp), "%s", wifi.getClientIp());
    if (previousState == TerminalWifi::State::ApprovalPending ||
        displayState == TerminalWifi::State::ApprovalPending) {
      screen.markAllDirty();
    }
  }

  lastNetworkGeneration = generation;
  connected.store(wifi.isConnected(), std::memory_order_release);
  statusDirty.store(true, std::memory_order_release);
  scheduleRender(false);
}

void TerminalActivity::scheduleRender(const bool forceFull) {
  if (forceFull) {
    forceFullRefresh.store(true, std::memory_order_release);
  }
  bool expected = false;
  if (renderScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    requestUpdate();
  }
}

void TerminalActivity::loop() {
  if (!terminalStarted) return;

  const uint32_t now = millis();
  wifi.poll();
  syncNetworkState();

  if (wifi.getState() == TerminalWifi::State::ApprovalPending) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      wifi.acceptRequest(TerminalScreen::COLS, TerminalScreen::ROWS);
      syncNetworkState();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        finish();
        return;
      }
      wifi.denyRequest();
      syncNetworkState();
    }
    return;
  }

  if (terminalInput.poll()) {
    finish();
    return;
  }

  pollWifi(now);

  if (!hasQueuedBytes()) {
    firstQueuedAt.store(0, std::memory_order_relaxed);
  } else {
    uint32_t first = firstQueuedAt.load(std::memory_order_relaxed);
    if (first == 0) {
      firstQueuedAt.store(now, std::memory_order_relaxed);
      first = now;
    }
    const uint32_t last = lastQueuedAt.load(std::memory_order_relaxed);
    if (now - last >= INTERACTIVE_BATCH_MS || now - first >= MAX_BATCH_MS) {
      scheduleRender(false);
    }
  }

  if (!renderScheduled.load(std::memory_order_acquire) && !hasQueuedBytes() &&
      fastRefreshCount.load(std::memory_order_relaxed) > 0 &&
      now - lastRenderAt.load(std::memory_order_relaxed) >= IDLE_CLEAN_MS) {
    scheduleRender(true);
  }
  if (statusDirty.load(std::memory_order_acquire)) {
    scheduleRender(false);
  }
}

void TerminalActivity::drainQueuedBytes() {
  if (rxOverflow.exchange(false, std::memory_order_acq_rel)) {
    for (const char byte : RX_OVERFLOW_NOTICE) {
      if (byte != '\0') {
        parser.feed(static_cast<uint8_t>(byte));
      }
    }
  }

  size_t tail = rxTail.load(std::memory_order_relaxed);
  const size_t limit = rxHead.load(std::memory_order_acquire);
  while (tail != limit) {
    parser.feed(rxRing[tail]);
    tail = (tail + 1) & RX_RING_MASK;
    rxTail.store(tail, std::memory_order_release);
  }
}

void TerminalActivity::drawStatus() {
  const char* stateText = tr(STR_KNIETTY_WIFI_OFFLINE);
  char connectedText[56]{};
  switch (displayState) {
    case TerminalWifi::State::Waiting:
      stateText = tr(STR_KNIETTY_WIFI_WAITING);
      break;
    case TerminalWifi::State::Negotiating:
      stateText = tr(STR_KNIETTY_WIFI_NEGOTIATING);
      break;
    case TerminalWifi::State::ApprovalPending:
      stateText = tr(STR_KNIETTY_WIFI_APPROVAL);
      break;
    case TerminalWifi::State::Connected:
      std::snprintf(connectedText, sizeof(connectedText), tr(STR_KNIETTY_WIFI_CONNECTED), displayClientName);
      stateText = connectedText;
      break;
    case TerminalWifi::State::Offline:
      break;
  }

  char status[96];
  std::snprintf(status, sizeof(status), tr(STR_KNIETTY_WIFI_STATUS), tr(STR_KNIETTY), stateText);
  renderer.fillRect(0, 0, renderer.getScreenWidth(), HEADER_HEIGHT, false);
  renderer.drawText(UI_10_FONT_ID, 8, 5, status);
  renderer.drawLine(0, HEADER_HEIGHT - 1, renderer.getScreenWidth() - 1, HEADER_HEIGHT - 1);
}

void TerminalActivity::drawApprovalPrompt() {
  char request[96];
  std::snprintf(request, sizeof(request), tr(STR_KNIETTY_REQUEST_FORMAT), displayClientName, displayClientIp);
  renderer.fillRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT, false);
  renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_KNIETTY_REQUEST_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 205, request);
  renderer.drawCenteredText(UI_10_FONT_ID, 275, tr(STR_KNIETTY_REQUEST_HINT));
}

void TerminalActivity::drawDirtyRows(const uint32_t dirtyRows) {
  for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) {
    if ((dirtyRows & (uint32_t{1} << row)) == 0) {
      continue;
    }
    for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
      const auto& cell = screen.getCell(row, column);
      const bool cursor =
          screen.isCursorVisible() && row == screen.getCursorRow() && column == screen.getCursorColumn();
      TerminalFont::drawCell(renderer, column * TerminalFont::CELL_WIDTH,
                             HEADER_HEIGHT + row * TerminalFont::CELL_HEIGHT, cell.character, cell.attributes, cursor);
    }
  }
}

void TerminalActivity::render(RenderLock&&) {
  if (!terminalStarted) return;
  drainQueuedBytes();

  if (firstRender) {
    renderer.clearScreen();
    screen.markAllDirty();
    statusDirty.store(true, std::memory_order_release);
  }

  if (statusDirty.exchange(false, std::memory_order_acq_rel)) {
    drawStatus();
  }
  drawDirtyRows(screen.takeDirtyRows());
  if (displayState == TerminalWifi::State::ApprovalPending) drawApprovalPrompt();

  const bool clean = forceFullRefresh.exchange(false, std::memory_order_acq_rel) ||
                     fastRefreshCount.load(std::memory_order_relaxed) >= FAST_REFRESH_LIMIT;
  const HalDisplay::RefreshMode refreshMode =
      firstRender ? HalDisplay::FULL_REFRESH : (clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  renderer.displayBuffer(refreshMode);

  const uint32_t now = millis();
  lastRenderAt.store(now, std::memory_order_relaxed);
  if (refreshMode != HalDisplay::FAST_REFRESH) {
    fastRefreshCount.store(0, std::memory_order_relaxed);
  } else {
    fastRefreshCount.fetch_add(1, std::memory_order_relaxed);
  }
  firstRender = false;
  renderScheduled.store(false, std::memory_order_release);
}

bool TerminalActivity::preventAutoSleep() {
  return terminalStarted && (WiFi.status() == WL_CONNECTED || connected.load(std::memory_order_acquire) ||
                             hasQueuedBytes() || millis() - lastTrafficAt.load() < 30000);
}
