#include "TerminalActivity.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "terminal/TerminalFont.h"
#include "terminal/TerminalLayout.h"

namespace {

void mergeDirtyRegion(TerminalScreen::DirtyRegion& destination, const TerminalScreen::DirtyRegion& source) {
  for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) {
    const uint32_t bit = uint32_t{1} << row;
    if ((source.rows & bit) == 0) continue;
    if ((destination.rows & bit) == 0) {
      destination.firstColumn[row] = source.firstColumn[row];
      destination.lastColumn[row] = source.lastColumn[row];
      destination.rows |= bit;
      continue;
    }
    destination.firstColumn[row] = std::min(destination.firstColumn[row], source.firstColumn[row]);
    destination.lastColumn[row] = std::max(destination.lastColumn[row], source.lastColumn[row]);
  }
}

bool dirtyBounds(const TerminalScreen::DirtyRegion& region, int& left, int& top, int& right, int& bottom) {
  left = TerminalLayout::SCREEN_WIDTH;
  top = TerminalLayout::TOP + TerminalScreen::ROWS * TerminalLayout::CELL_HEIGHT;
  right = 0;
  bottom = 0;
  for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) {
    if ((region.rows & (uint32_t{1} << row)) == 0) continue;
    left = std::min(left, TerminalLayout::columnX(region.firstColumn[row]));
    top = std::min(top, TerminalLayout::TOP + row * TerminalLayout::CELL_HEIGHT);
    right = std::max(right, TerminalLayout::columnX(region.lastColumn[row] + 1));
    bottom = std::max(bottom, TerminalLayout::TOP + (row + 1) * TerminalLayout::CELL_HEIGHT);
  }
  return right > left && bottom > top;
}

}  // namespace

void TerminalActivity::RefreshMetrics::recordInteractive(const uint32_t total, const uint32_t render,
                                                         const uint32_t queue,
                                                         const HalDisplay::RefreshTiming& displayTiming,
                                                         const bool windowed, const uint16_t windowWidth,
                                                         const uint16_t windowHeight) {
  ++count;
  lastTotalUs = total;
  lastRenderUs = render;
  lastTransferUs = displayTiming.transferUs;
  lastWaveformUs = displayTiming.waveformUs;
  lastQueueUs = queue;
  lastLutUs = displayTiming.lutUs;
  lastPlaneUs = displayTiming.planeUs;
  lastBaselineUs = displayTiming.baselineUs;
  lastWindowWidth = windowWidth;
  lastWindowHeight = windowHeight;
  lastWindowBytes = (static_cast<uint32_t>(windowWidth) * windowHeight + 7) / 8;
  minTotalUs = std::min(minTotalUs, total);
  maxTotalUs = std::max(maxTotalUs, total);
  totalUs += total;
  if (windowed) {
    ++windowedCount;
  } else {
    ++fallbackCount;
  }
}

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

#ifdef KNIETTY_ADAPTIVE_REFRESH
  renderer.setFastRefreshProfile(HalDisplay::FastRefreshProfile::TerminalInteractive);
#else
  renderer.setFastRefreshProfile(HalDisplay::FastRefreshProfile::PanelDefault);
#endif

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
  clearContentArea.store(false);
  displayState = wifi.getState();
  displayClientName[0] = '\0';
  displayClientIp[0] = '\0';
  displayClock[0] = '\0';
  // Seed the first E Ink frame with a real reading. Waiting for loop() left a
  // visible 0% behind for an entire refresh on otherwise healthy batteries.
  displayBattery = powerManager.getBatteryPercentage();
  std::snprintf(displayHostname, sizeof(displayHostname), "%s", wifi.getHostname());
  std::snprintf(displayLocalIp, sizeof(displayLocalIp), "%s", wifi.getLocalIp());
  lastNetworkGeneration = wifi.getGeneration();
  statusDirty.store(true);
  forceFullRefresh.store(true);
  firstQueuedAt.store(0);
  lastQueuedAt.store(0);
  exitConfirmUntil = 0;
  exitConfirmationArmed = false;
  terminalInverted = false;
  renderInverted = false;
  framebufferInverted = false;
  waitingDiagnostics = false;
  renderWaitingDiagnostics = false;
  refreshMetrics = {};
  renderRefreshMetrics = {};
#ifdef KNIETTY_ADAPTIVE_REFRESH
  settleRegion = {};
  settleRequested.store(false);
  cleanRequested.store(false);
  settleDebtPending.store(false);
  cleanDebt.store(0);
#endif
  firstRender = true;
  renderScheduled.store(true);
  requestUpdate();
}

void TerminalActivity::onExit() {
  wifi.end();
  // ActivityManager calls onExit while holding RenderLock, so restoring the
  // shared orientation here is synchronized with any in-flight render.
  if (terminalStarted) {
    renderer.setFastRefreshProfile(HalDisplay::FastRefreshProfile::PanelDefault);
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
    std::snprintf(displayHostname, sizeof(displayHostname), "%s", wifi.getHostname());
    std::snprintf(displayLocalIp, sizeof(displayLocalIp), "%s", wifi.getLocalIp());
    if (previousState != displayState) {
      clearContentArea.store(true, std::memory_order_release);
    }
    if (previousState == TerminalWifi::State::Connected || displayState == TerminalWifi::State::Connected ||
        previousState == TerminalWifi::State::ApprovalPending || displayState == TerminalWifi::State::ApprovalPending) {
      screen.markAllDirty();
      contentDirty.store(true, std::memory_order_release);
    }
    if (previousState == TerminalWifi::State::Connected && displayState != TerminalWifi::State::Connected) {
      forceFullRefresh.store(true, std::memory_order_release);
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
  // Repaint the complete terminal through the active FAST profile. The old
  // forced HALF here caused the black/white flash users saw on every polarity
  // change.
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

  if (wifi.getState() != TerminalWifi::State::Connected &&
      (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
       mappedInput.wasPressed(MappedInputManager::Button::Right))) {
    waitingDiagnostics.store(!waitingDiagnostics.load(std::memory_order_relaxed), std::memory_order_relaxed);
    clearContentArea.store(true, std::memory_order_release);
    scheduleRender(false);
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

#ifdef KNIETTY_ADAPTIVE_REFRESH
  if (wifi.getState() == TerminalWifi::State::Connected && !contentDirty.load(std::memory_order_acquire)) {
    const uint32_t last = lastQueuedAt.load(std::memory_order_relaxed);
    if (last != 0 && cleanDebt.load(std::memory_order_relaxed) >= CLEAN_DEBT_LIMIT && now - last >= CLEAN_QUIET_MS) {
      cleanRequested.store(true, std::memory_order_release);
      scheduleRender(false);
    } else if (last != 0 && settleDebtPending.load(std::memory_order_acquire) && now - last >= SETTLE_QUIET_MS) {
      settleRequested.store(true, std::memory_order_release);
      scheduleRender(false);
    }
  }
#endif
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
  constexpr int sidePadding = 6;
  constexpr int batteryWidth = 16;
  constexpr int batteryHeight = 12;
  constexpr int batteryNubWidth = 2;
  constexpr int itemGap = 5;
  const int batteryX = renderer.getScreenWidth() - sidePadding - batteryWidth - batteryNubWidth;
  const int batteryY = (HEADER_HEIGHT - batteryHeight) / 2;
  const int textY = batteryY;
  renderer.drawRect(batteryX, batteryY, batteryWidth, batteryHeight);
  renderer.fillRect(batteryX + batteryWidth, batteryY + 4, batteryNubWidth, 4);
  GUI.fillBatteryIcon(renderer, Rect{batteryX, batteryY, batteryWidth, batteryHeight}, renderBattery);

  char batteryText[8];
  std::snprintf(batteryText, sizeof(batteryText), "%u%%", static_cast<unsigned>(renderBattery));
  const int batteryTextWidth = renderer.getTextWidth(SMALL_FONT_ID, batteryText);
  const int batteryTextX = batteryX - itemGap - batteryTextWidth;
  renderer.drawText(SMALL_FONT_ID, batteryTextX, textY, batteryText);

  if (renderClock[0] != '\0') {
    renderer.drawCenteredText(SMALL_FONT_ID, textY, renderClock);
  }
  renderer.drawText(SMALL_FONT_ID, sidePadding, textY, status);
  renderer.drawLine(0, HEADER_HEIGHT - 1, renderer.getScreenWidth() - 1, HEADER_HEIGHT - 1);
}

void TerminalActivity::drawContextualHints(const MappedInputManager::Labels& labels) {
  // CrossPoint's standard hint renderer deliberately switches to portrait
  // coordinates. With the terminal in landscape-counter-clockwise this puts
  // the physical button labels, rotated, on the right edge of the panel.
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TerminalActivity::drawWaitingScreen() {
  renderer.fillRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT, false);

  if (renderWaitingDiagnostics) {
    drawRefreshDiagnostics();
    const auto labels =
        mappedInput.mapLabels(tr(STR_KNIETTY_HOLD_INVERT), "", tr(STR_KNIETTY_TIPS), tr(STR_KNIETTY_TIPS));
    drawContextualHints(labels);
    return;
  }

  char address[96];
  const char* localIp = renderLocalIp[0] == '\0' ? "offline" : renderLocalIp;
  std::snprintf(address, sizeof(address), "%s  %s:%u", renderHostname, localIp,
                static_cast<unsigned>(TerminalWifi::PORT));

  renderer.drawCenteredText(UI_12_FONT_ID, 70, tr(STR_KNIETTY_READY_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 112, address);
  renderer.drawCenteredText(SMALL_FONT_ID, 146, "knietty --host auto");

  constexpr int labelX = 205;
  constexpr int valueX = 390;
  constexpr int firstY = 205;
  constexpr int rowStep = 38;
  renderer.drawText(UI_10_FONT_ID, labelX, firstY, tr(STR_KNIETTY_POWER_TWICE));
  renderer.drawText(UI_10_FONT_ID, valueX, firstY, tr(STR_KNIETTY_EXIT_ACTION));
  renderer.drawText(UI_10_FONT_ID, labelX, firstY + rowStep, tr(STR_KNIETTY_HOLD_BACK));
  renderer.drawText(UI_10_FONT_ID, valueX, firstY + rowStep, tr(STR_KNIETTY_INVERT_ACTION));
  renderer.drawText(UI_10_FONT_ID, labelX, firstY + rowStep * 2, tr(STR_KNIETTY_CONFIRM_CONTROL));
  renderer.drawText(UI_10_FONT_ID, valueX, firstY + rowStep * 2, tr(STR_KNIETTY_ENTER_CTRLC));
  renderer.drawText(UI_10_FONT_ID, labelX, firstY + rowStep * 3, tr(STR_KNIETTY_ARROWS_CONTROL));
  renderer.drawText(UI_10_FONT_ID, valueX, firstY + rowStep * 3, tr(STR_KNIETTY_NAVIGATE_ACTION));
  renderer.drawText(UI_10_FONT_ID, labelX, firstY + rowStep * 4, tr(STR_KNIETTY_LEFT_RIGHT));
  renderer.drawText(UI_10_FONT_ID, valueX, firstY + rowStep * 4, tr(STR_KNIETTY_TIMING));

  const auto labels =
      mappedInput.mapLabels(tr(STR_KNIETTY_HOLD_INVERT), "", tr(STR_KNIETTY_TIMING), tr(STR_KNIETTY_TIMING));
  drawContextualHints(labels);
}

void TerminalActivity::drawRefreshDiagnostics() {
  renderer.drawCenteredText(UI_12_FONT_ID, 70, tr(STR_KNIETTY_TIMING_TITLE), true, EpdFontFamily::BOLD);
#if defined(KNIETTY_ADAPTIVE_REFRESH) && defined(FREEINK_X4_OVERCLOCK_SPI)
  renderer.drawCenteredText(UI_10_FONT_ID, 110, "Adaptive DU / 40 MHz experimental");
#elif defined(KNIETTY_ADAPTIVE_REFRESH)
  renderer.drawCenteredText(UI_10_FONT_ID, 110, "Adaptive DU / 20 MHz experimental");
#else
  renderer.drawCenteredText(UI_10_FONT_ID, 110, "Stock X4 partial / 20 MHz safe");
#endif

  if (renderRefreshMetrics.count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 190, tr(STR_KNIETTY_TIMING_EMPTY));
    return;
  }

  const auto millisTenths = [](const uint32_t microsValue, char* output, const size_t outputSize) {
    std::snprintf(output, outputSize, "%lu.%01lu ms", static_cast<unsigned long>(microsValue / 1000),
                  static_cast<unsigned long>((microsValue % 1000) / 100));
  };

  char last[20];
  char waveform[20];
  char transfer[20];
  char render[20];
  char queue[20];
  char lut[20];
  char plane[20];
  char baseline[20];
  char average[20];
  char minimum[20];
  char maximum[20];
  millisTenths(renderRefreshMetrics.lastTotalUs, last, sizeof(last));
  millisTenths(renderRefreshMetrics.lastWaveformUs, waveform, sizeof(waveform));
  millisTenths(renderRefreshMetrics.lastTransferUs, transfer, sizeof(transfer));
  millisTenths(renderRefreshMetrics.lastRenderUs, render, sizeof(render));
  millisTenths(renderRefreshMetrics.lastQueueUs, queue, sizeof(queue));
  millisTenths(renderRefreshMetrics.lastLutUs, lut, sizeof(lut));
  millisTenths(renderRefreshMetrics.lastPlaneUs, plane, sizeof(plane));
  millisTenths(renderRefreshMetrics.lastBaselineUs, baseline, sizeof(baseline));
  millisTenths(static_cast<uint32_t>(renderRefreshMetrics.totalUs / renderRefreshMetrics.count), average,
               sizeof(average));
  millisTenths(renderRefreshMetrics.minTotalUs, minimum, sizeof(minimum));
  millisTenths(renderRefreshMetrics.maxTotalUs, maximum, sizeof(maximum));

  char line[112];
  std::snprintf(line, sizeof(line), "Last %s   waveform %s", last, waveform);
  renderer.drawCenteredText(UI_10_FONT_ID, 165, line);
  std::snprintf(line, sizeof(line), "Queue %s   render %s", queue, render);
  renderer.drawCenteredText(UI_10_FONT_ID, 205, line);
  std::snprintf(line, sizeof(line), "Transfer %s: plane %s LUT %s base %s", transfer, plane, lut, baseline);
  renderer.drawCenteredText(UI_10_FONT_ID, 245, line);
  std::snprintf(line, sizeof(line), "Average %s   min %s   max %s", average, minimum, maximum);
  renderer.drawCenteredText(UI_10_FONT_ID, 285, line);
  std::snprintf(line, sizeof(line), "Updates %lu window %lu fallback %lu settle %lu clean %lu",
                static_cast<unsigned long>(renderRefreshMetrics.count),
                static_cast<unsigned long>(renderRefreshMetrics.windowedCount),
                static_cast<unsigned long>(renderRefreshMetrics.fallbackCount),
                static_cast<unsigned long>(renderRefreshMetrics.settleCount),
                static_cast<unsigned long>(renderRefreshMetrics.cleanCount));
  renderer.drawCenteredText(UI_10_FONT_ID, 325, line);
  std::snprintf(line, sizeof(line), "Last region %ux%u / %lu bytes", renderRefreshMetrics.lastWindowWidth,
                renderRefreshMetrics.lastWindowHeight,
                static_cast<unsigned long>(renderRefreshMetrics.lastWindowBytes));
  renderer.drawCenteredText(UI_10_FONT_ID, 365, line);
  renderer.drawCenteredText(SMALL_FONT_ID, 405, tr(STR_KNIETTY_TIMING_NOTE));
}

void TerminalActivity::drawApprovalPrompt() {
  char request[96];
  std::snprintf(request, sizeof(request), tr(STR_KNIETTY_REQUEST_FORMAT), renderClientName, renderClientIp);
  renderer.fillRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT, false);
  renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_KNIETTY_REQUEST_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 205, request);
  renderer.drawCenteredText(SMALL_FONT_ID, 250, tr(STR_KNIETTY_REQUEST_HINT));
  const auto labels = mappedInput.mapLabels(tr(STR_KNIETTY_DENY), tr(STR_KNIETTY_ACCEPT), "", "");
  drawContextualHints(labels);
}

void TerminalActivity::drawDirtyCells(const TerminalScreen::DirtyRegion& dirtyRegion) {
  for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) {
    if ((dirtyRegion.rows & (uint32_t{1} << row)) == 0) continue;
    for (uint8_t column = dirtyRegion.firstColumn[row]; column <= dirtyRegion.lastColumn[row]; ++column) {
      const auto& cell = renderScreen.getCell(row, column);
      const bool cursor = renderScreen.isCursorVisible() && row == renderScreen.getCursorRow() &&
                          column == renderScreen.getCursorColumn();
      TerminalFont::drawCell(renderer, TerminalLayout::columnX(column),
                             TerminalLayout::TOP + row * TerminalLayout::CELL_HEIGHT,
                             TerminalLayout::columnWidth(column), cell.codepoint, cell.attributes, cursor);
    }
  }
}

void TerminalActivity::render(RenderLock&&) {
  if (!terminalStarted) return;
  const uint32_t renderStartedAtMs = millis();
  const uint32_t renderStartedAtUs = micros();

  // Previous inverted frames are kept inverted between paints so the physical
  // framebuffer mirrors the panel. Restore the logical black-on-white frame
  // before applying this render's dirty changes.
  if (framebufferInverted) {
    renderer.invertScreen();
    framebufferInverted = false;
  }

  const bool shouldDrawStatus = statusDirty.exchange(false, std::memory_order_acq_rel);
  const bool shouldClearContent = clearContentArea.exchange(false, std::memory_order_acq_rel);
  TerminalScreen::DirtyRegion dirtyRegion;
  uint32_t queuedAtMs = 0;
  {
    std::lock_guard<std::mutex> lock(modelMutex);
    if (firstRender) screen.markAllDirty();
    renderScreen = screen;
    dirtyRegion = screen.takeDirtyRegion();
    queuedAtMs = firstQueuedAt.exchange(0, std::memory_order_acq_rel);
    contentDirty.store(false, std::memory_order_release);
    renderDisplayState = displayState;
    std::snprintf(renderClientName, sizeof(renderClientName), "%s", displayClientName);
    std::snprintf(renderClientIp, sizeof(renderClientIp), "%s", displayClientIp);
    std::snprintf(renderClock, sizeof(renderClock), "%s", displayClock);
    renderBattery = displayBattery > 100 ? 0 : displayBattery;
    std::snprintf(renderHostname, sizeof(renderHostname), "%s", displayHostname);
    std::snprintf(renderLocalIp, sizeof(renderLocalIp), "%s", displayLocalIp);
    renderRefreshMetrics = refreshMetrics;
    renderExitConfirmation = exitConfirmationArmed;
    renderWaitingDiagnostics = waitingDiagnostics.load(std::memory_order_relaxed);
    renderInverted = terminalInverted;
  }

  if (firstRender) {
    renderer.clearScreen();
  }
  if (shouldClearContent) {
    renderer.fillRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT, false);
  }
  if (firstRender || shouldDrawStatus) {
    drawStatus();
  }
  if (renderDisplayState == TerminalWifi::State::Connected) {
    drawDirtyCells(dirtyRegion);
  } else if (renderDisplayState == TerminalWifi::State::ApprovalPending) {
    drawApprovalPrompt();
  } else {
    drawWaitingScreen();
  }

  if (renderInverted) {
    renderer.invertScreen();
    framebufferInverted = true;
  }

  const bool connected = renderDisplayState == TerminalWifi::State::Connected;
  const bool hasTerminalChanges = connected && !dirtyRegion.empty();
#ifdef KNIETTY_ADAPTIVE_REFRESH
  const bool requestedClean = cleanRequested.exchange(false, std::memory_order_acq_rel);
  const bool requestedSettle = settleRequested.exchange(false, std::memory_order_acq_rel);
  const bool settle = connected && requestedSettle && !hasTerminalChanges && !settleRegion.empty();
#else
  constexpr bool requestedClean = false;
  constexpr bool settle = false;
#endif
  const bool clean = forceFullRefresh.exchange(false, std::memory_order_acq_rel) || requestedClean;
  HalDisplay::RefreshMode refreshMode = clean || firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  bool usedWindow = false;
  int updateLeft = renderer.getScreenWidth();
  int updateTop = renderer.getScreenHeight();
  int updateRight = 0;
  int updateBottom = 0;
  if (refreshMode == HalDisplay::FAST_REFRESH) {
    const auto includeRect = [&](const int x, const int y, const int width, const int height) {
      updateLeft = std::min(updateLeft, x);
      updateTop = std::min(updateTop, y);
      updateRight = std::max(updateRight, x + width);
      updateBottom = std::max(updateBottom, y + height);
    };
#ifdef KNIETTY_ADAPTIVE_REFRESH
    if (settle) {
      dirtyBounds(settleRegion, updateLeft, updateTop, updateRight, updateBottom);
    } else {
#endif
      if (shouldDrawStatus) includeRect(0, 0, renderer.getScreenWidth(), HEADER_HEIGHT);
      if (shouldClearContent || !connected) {
        includeRect(0, HEADER_HEIGHT, renderer.getScreenWidth(), renderer.getScreenHeight() - HEADER_HEIGHT);
      } else {
        int dirtyLeft = 0;
        int dirtyTop = 0;
        int dirtyRight = 0;
        int dirtyBottom = 0;
        if (dirtyBounds(dirtyRegion, dirtyLeft, dirtyTop, dirtyRight, dirtyBottom)) {
          includeRect(dirtyLeft, dirtyTop, dirtyRight - dirtyLeft, dirtyBottom - dirtyTop);
        }
      }
#ifdef KNIETTY_ADAPTIVE_REFRESH
    }
#endif
    if (updateRight > updateLeft && updateBottom > updateTop) {
#ifdef KNIETTY_ADAPTIVE_REFRESH
      const bool terminalOutputUpdate = hasTerminalChanges && queuedAtMs != 0 && !settle;
      renderer.setFastRefreshProfile(terminalOutputUpdate ? HalDisplay::FastRefreshProfile::TerminalInteractive
                                     : settle             ? HalDisplay::FastRefreshProfile::TerminalSettle
                                                          : HalDisplay::FastRefreshProfile::PanelDefault);
#endif
      usedWindow = renderer.displayWindow(updateLeft, updateTop, updateRight - updateLeft, updateBottom - updateTop);
    }
  } else {
#ifdef KNIETTY_ADAPTIVE_REFRESH
    renderer.setFastRefreshProfile(HalDisplay::FastRefreshProfile::PanelDefault);
#endif
    renderer.displayBuffer(refreshMode);
  }
#ifdef KNIETTY_ADAPTIVE_REFRESH
  renderer.setFastRefreshProfile(HalDisplay::FastRefreshProfile::TerminalInteractive);
#endif

  const auto displayTiming = renderer.getLastRefreshTiming();
  const uint32_t renderAndDisplayUs = micros() - renderStartedAtUs;
  const uint32_t renderUs =
      renderAndDisplayUs >= displayTiming.totalUs ? renderAndDisplayUs - displayTiming.totalUs : 0;
  const bool terminalOutputUpdate = hasTerminalChanges && queuedAtMs != 0 && !clean && !settle;
  const uint32_t queueUs = terminalOutputUpdate ? (renderStartedAtMs - queuedAtMs) * 1000 : 0;
  {
    std::lock_guard<std::mutex> lock(modelMutex);
    if (terminalOutputUpdate) {
      refreshMetrics.recordInteractive(queueUs + renderAndDisplayUs, renderUs, queueUs, displayTiming, usedWindow,
                                       static_cast<uint16_t>(std::max(0, updateRight - updateLeft)),
                                       static_cast<uint16_t>(std::max(0, updateBottom - updateTop)));
    }
#ifdef KNIETTY_ADAPTIVE_REFRESH
    if (clean && connected) {
      ++refreshMetrics.cleanCount;
      settleRegion = {};
      settleDebtPending.store(false, std::memory_order_release);
      cleanDebt.store(0, std::memory_order_relaxed);
    } else if (settle) {
      ++refreshMetrics.settleCount;
      settleRegion = {};
      settleDebtPending.store(false, std::memory_order_release);
    } else if (terminalOutputUpdate) {
      mergeDirtyRegion(settleRegion, dirtyRegion);
      settleDebtPending.store(true, std::memory_order_release);
      cleanDebt.fetch_add(1, std::memory_order_relaxed);
    }
#endif
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
