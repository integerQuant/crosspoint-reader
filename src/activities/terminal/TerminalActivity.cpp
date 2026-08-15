#include "TerminalActivity.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "terminal/TerminalFont.h"

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
    std::lock_guard<std::mutex> modelLock(modelMutex);
    screen.reset();
    renderScreen.reset();
    parser.reset();
    terminalStarted = true;
  }

  contentDirty.store(true);
  displayState = wifi.getState();
  displayClientName[0] = '\0';
  displayClientIp[0] = '\0';
  displayClock[0] = '\0';
  displayBattery = 101;
  lastNetworkGeneration = wifi.getGeneration();
  statusDirty.store(true);
  forceFullRefresh.store(true);
  firstQueuedAt.store(0);
  lastQueuedAt.store(0);
  fastRefreshCount.store(0);
  exitConfirmUntil = 0;
  exitConfirmationArmed = false;
  terminalInverted = false;
  renderInverted = false;
  framebufferInverted = false;
  firstRender = true;
  renderScheduled.store(true);
  requestUpdate();
}

void TerminalActivity::onExit() {
  wifi.end();
  // ActivityManager calls onExit while holding RenderLock, so restoring the
  // shared orientation here is synchronized with any in-flight render.
  if (terminalStarted) {
    if (framebufferInverted) {
      renderer.invertScreen();
      framebufferInverted = false;
    }
    renderer.setOrientation(previousOrientation);
  }
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void TerminalActivity::pollWifi(const uint32_t now) {
  uint8_t received[256];
  size_t receivedCount = 0;
  for (; receivedCount < sizeof(received) && wifi.available() > 0; ++receivedCount) {
    const int byte = wifi.read();
    if (byte < 0) {
      break;
    }
    received[receivedCount] = static_cast<uint8_t>(byte);
  }

  if (receivedCount > 0) {
    bool dirty = false;
    {
      // Parsing is deliberately independent of the E Ink refresh lock. The
      // render task snapshots this model briefly, then the main task remains
      // free to consume TCP while the panel waveform is active.
      std::lock_guard<std::mutex> lock(modelMutex);
      for (size_t i = 0; i < receivedCount; ++i) parser.feed(received[i]);
      dirty = screen.hasDirtyRows();
      if (dirty) contentDirty.store(true, std::memory_order_release);
    }
    if (dirty) {
      lastQueuedAt.store(now, std::memory_order_relaxed);
      uint32_t expected = 0;
      firstQueuedAt.compare_exchange_strong(expected, now, std::memory_order_relaxed);
    }
  }
}

void TerminalActivity::syncNetworkState() {
  const uint32_t generation = wifi.getGeneration();
  if (generation == lastNetworkGeneration) return;

  {
    std::lock_guard<std::mutex> lock(modelMutex);
    const TerminalWifi::State previousState = displayState;
    displayState = wifi.getState();
    std::snprintf(displayClientName, sizeof(displayClientName), "%s", wifi.getClientName());
    std::snprintf(displayClientIp, sizeof(displayClientIp), "%s", wifi.getClientIp());
    if (previousState == TerminalWifi::State::ApprovalPending ||
        displayState == TerminalWifi::State::ApprovalPending) {
      screen.markAllDirty();
      contentDirty.store(true, std::memory_order_release);
    }
  }

  lastNetworkGeneration = generation;
  statusDirty.store(true, std::memory_order_release);
  scheduleRender(false);
}

void TerminalActivity::syncClock() {
  char clock[sizeof(displayClock)]{};
  wifi.formatHostTime(clock, sizeof(clock));
  const uint16_t battery = powerManager.getBatteryPercentage();
  std::lock_guard<std::mutex> lock(modelMutex);
  if (std::strncmp(displayClock, clock, sizeof(displayClock)) == 0 && displayBattery == battery) return;
  std::snprintf(displayClock, sizeof(displayClock), "%s", clock);
  displayBattery = battery;
  statusDirty.store(true, std::memory_order_release);
}

bool TerminalActivity::handlePowerButton(const uint32_t now) {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power)) return false;
  if (exitConfirmationArmed && static_cast<int32_t>(exitConfirmUntil - now) >= 0) {
    finish();
    return true;
  }
  exitConfirmationArmed = true;
  exitConfirmUntil = now + EXIT_CONFIRM_MS;
  statusDirty.store(true, std::memory_order_release);
  scheduleRender(false);
  return false;
}

void TerminalActivity::toggleInversion() {
  {
    std::lock_guard<std::mutex> lock(modelMutex);
    terminalInverted = !terminalInverted;
    screen.markAllDirty();
  }
  contentDirty.store(true, std::memory_order_release);
  statusDirty.store(true, std::memory_order_release);
  scheduleRender(true);
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
  syncClock();

  if (exitConfirmationArmed && static_cast<int32_t>(now - exitConfirmUntil) >= 0) {
    exitConfirmationArmed = false;
    statusDirty.store(true, std::memory_order_release);
    scheduleRender(false);
  }
  if (handlePowerButton(now)) return;

  if (wifi.getState() == TerminalWifi::State::ApprovalPending) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      wifi.acceptRequest(TerminalScreen::COLS, TerminalScreen::ROWS);
      syncNetworkState();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      wifi.denyRequest();
      syncNetworkState();
    }
    return;
  }

  if (terminalInput.poll()) {
    toggleInversion();
  }

  pollWifi(now);

  if (!contentDirty.load(std::memory_order_acquire)) {
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

  if (statusDirty.load(std::memory_order_acquire)) {
    scheduleRender(false);
  }
}

void TerminalActivity::drawStatus() {
  char status[56]{};
  if (renderExitConfirmation) {
    std::snprintf(status, sizeof(status), "%s", tr(STR_KNIETTY_EXIT_CONFIRM));
  } else if ((renderDisplayState == TerminalWifi::State::Connected ||
              renderDisplayState == TerminalWifi::State::ApprovalPending) &&
             renderClientName[0] != '\0') {
    std::snprintf(status, sizeof(status), "%s@%s", tr(STR_KNIETTY), renderClientName);
  } else if (renderDisplayState == TerminalWifi::State::Offline) {
    std::snprintf(status, sizeof(status), "%s@offline", tr(STR_KNIETTY));
  } else {
    std::snprintf(status, sizeof(status), "%s@waiting", tr(STR_KNIETTY));
  }

  renderer.fillRect(0, 0, renderer.getScreenWidth(), HEADER_HEIGHT, false);
  constexpr int sidePadding = 12;
  constexpr int textY = 6;
  constexpr int batteryWidth = 16;
  constexpr int batteryHeight = 12;
  constexpr int batteryNubWidth = 2;
  constexpr int itemGap = 8;
  const int batteryX = renderer.getScreenWidth() - sidePadding - batteryWidth - batteryNubWidth;
  const int batteryY = (HEADER_HEIGHT - batteryHeight) / 2;
  renderer.drawRect(batteryX, batteryY, batteryWidth, batteryHeight);
  renderer.fillRect(batteryX + batteryWidth, batteryY + 4, batteryNubWidth, 4);
  GUI.fillBatteryIcon(renderer, Rect{batteryX, batteryY, batteryWidth, batteryHeight}, renderBattery);

  char batteryText[8];
  std::snprintf(batteryText, sizeof(batteryText), "%u%%", static_cast<unsigned>(renderBattery));
  const int batteryTextWidth = renderer.getTextWidth(SMALL_FONT_ID, batteryText);
  const int batteryTextX = batteryX - itemGap - batteryTextWidth;
  renderer.drawText(SMALL_FONT_ID, batteryTextX, textY, batteryText);

  if (renderClock[0] != '\0') {
    const int clockWidth = renderer.getTextWidth(SMALL_FONT_ID, renderClock);
    renderer.drawText(SMALL_FONT_ID, batteryTextX - itemGap - clockWidth, textY, renderClock);
  }
  renderer.drawText(SMALL_FONT_ID, sidePadding, textY, status);
  renderer.drawLine(0, HEADER_HEIGHT - 1, renderer.getScreenWidth() - 1, HEADER_HEIGHT - 1);
}

void TerminalActivity::drawApprovalPrompt() {
  char request[96];
  std::snprintf(request, sizeof(request), tr(STR_KNIETTY_REQUEST_FORMAT), renderClientName, renderClientIp);
  renderer.fillRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT, false);
  renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_KNIETTY_REQUEST_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 205, request);
  const auto labels = mappedInput.mapLabels(tr(STR_KNIETTY_DENY), tr(STR_KNIETTY_ACCEPT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TerminalActivity::drawDirtyRows(const uint32_t dirtyRows) {
  for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) {
    if ((dirtyRows & (uint32_t{1} << row)) == 0) {
      continue;
    }
    for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
      const auto& cell = renderScreen.getCell(row, column);
      const bool cursor =
          renderScreen.isCursorVisible() && row == renderScreen.getCursorRow() && column == renderScreen.getCursorColumn();
      TerminalFont::drawCell(renderer, TERMINAL_LEFT + column * TerminalFont::CELL_WIDTH,
                             TERMINAL_TOP + row * TerminalFont::CELL_HEIGHT, cell.character, cell.attributes, cursor);
    }
  }
}

void TerminalActivity::render(RenderLock&&) {
  if (!terminalStarted) return;

  // Previous inverted frames are kept inverted between paints so the physical
  // framebuffer mirrors the panel. Restore the logical black-on-white frame
  // before applying this render's dirty changes.
  if (framebufferInverted) {
    renderer.invertScreen();
    framebufferInverted = false;
  }

  const bool shouldDrawStatus = statusDirty.exchange(false, std::memory_order_acq_rel);
  uint32_t dirtyRows = 0;
  {
    std::lock_guard<std::mutex> lock(modelMutex);
    if (firstRender) screen.markAllDirty();
    renderScreen = screen;
    dirtyRows = screen.takeDirtyRows();
    contentDirty.store(false, std::memory_order_release);
    renderDisplayState = displayState;
    std::snprintf(renderClientName, sizeof(renderClientName), "%s", displayClientName);
    std::snprintf(renderClientIp, sizeof(renderClientIp), "%s", displayClientIp);
    std::snprintf(renderClock, sizeof(renderClock), "%s", displayClock);
    renderBattery = displayBattery > 100 ? 0 : displayBattery;
    renderExitConfirmation = exitConfirmationArmed;
    renderInverted = terminalInverted;
  }

  if (firstRender) {
    renderer.clearScreen();
  }
  if (firstRender || shouldDrawStatus) {
    drawStatus();
  }
  drawDirtyRows(dirtyRows);
  if (renderDisplayState == TerminalWifi::State::ApprovalPending) drawApprovalPrompt();

  if (renderInverted) {
    renderer.invertScreen();
    framebufferInverted = true;
  }

  const bool clean = forceFullRefresh.exchange(false, std::memory_order_acq_rel) ||
                     fastRefreshCount.load(std::memory_order_relaxed) >= FAST_REFRESH_LIMIT;
  const HalDisplay::RefreshMode refreshMode =
      firstRender ? HalDisplay::FULL_REFRESH : (clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  renderer.displayBuffer(refreshMode);

  if (refreshMode != HalDisplay::FAST_REFRESH) {
    fastRefreshCount.store(0, std::memory_order_relaxed);
  } else {
    fastRefreshCount.fetch_add(1, std::memory_order_relaxed);
  }
  firstRender = false;
  renderScheduled.store(false, std::memory_order_release);
}

bool TerminalActivity::preventAutoSleep() {
  // Deep sleep while the WiFi terminal owns the display/network currently
  // cannot resume safely on the X4. Exit knietty first, then let CrossPoint's
  // normal home/reader power behavior take over.
  return terminalStarted;
}
