#pragma once

#include <cstddef>
#include <cstdint>

#include "TerminalScreen.h"

namespace knietty::diagnostics {

constexpr uint8_t SCHEMA_VERSION = 1;
constexpr size_t MAX_COMMAND_PAYLOAD = 3;
constexpr size_t REFRESH_EVENT_PAYLOAD_SIZE = 108;

enum class Command : uint8_t {
  SessionInfo = 1,
  Reset = 2,
  Pattern = 3,
  SetPolarity = 4,
  Clean = 5,
  Stop = 6,
};

enum class Pattern : uint8_t {
  Cell = 1,
  Cursor = 2,
  Row = 3,
  DisjointRows = 4,
  Scroll = 5,
  Checker = 6,
  Full = 7,
  CellMiddle = 8,
  CellBottom = 9,
  AdjacentCells = 10,
  BoundaryUnder = 11,
  BoundaryOver = 12,
  Burst1 = 13,
  Burst2 = 14,
  Burst5 = 15,
  Burst10 = 16,
  Burst25 = 17,
  Burst100 = 18,
};

enum class Status : uint8_t { Accepted = 0, Rejected = 1 };

enum class Error : uint8_t {
  None = 0,
  Malformed = 1,
  UnknownCommand = 2,
  InvalidArgument = 3,
  CommandLimit = 4,
  ActivationLimit = 5,
  Timeout = 6,
  Busy = 7,
  Transport = 8,
  Aborted = 9,
};

enum class EventPhase : uint8_t { Presented = 1, Ready = 2, Failed = 3 };
enum class RefreshPath : uint8_t { None = 0, WindowFast = 1, FallbackFast = 2, Half = 3 };
enum class FallbackReason : uint8_t { None = 0, UnsupportedOrLarge = 1 };

struct Request {
  Command command = Command::SessionInfo;
  Pattern pattern = Pattern::Cell;
  uint8_t variant = 0;

  bool causesRefresh() const;
  bool forceClean() const { return command == Command::Clean; }
};

struct RefreshEvent {
  EventPhase phase = EventPhase::Ready;
  Command command = Command::SessionInfo;
  RefreshPath requestedPath = RefreshPath::None;
  RefreshPath actualPath = RefreshPath::None;
  FallbackReason fallbackReason = FallbackReason::None;
  uint8_t flags = 0;
  uint8_t queueDepth = 0;
  uint32_t timestampUs = 0;
  uint32_t rxAtUs = 0;
  uint32_t parsedAtUs = 0;
  uint32_t queuedAtUs = 0;
  uint32_t renderStartedAtUs = 0;
  uint32_t queueUs = 0;
  uint32_t renderUs = 0;
  uint32_t transferUs = 0;
  uint32_t lutUs = 0;
  uint32_t planeUs = 0;
  uint32_t activationToBusyUs = 0;
  uint32_t waveformUs = 0;
  uint32_t baselineUs = 0;
  uint32_t powerOffUs = 0;
  uint32_t totalUs = 0;
  uint16_t logicalX = 0;
  uint16_t logicalY = 0;
  uint16_t logicalWidth = 0;
  uint16_t logicalHeight = 0;
  uint16_t alignedX = 0;
  uint16_t alignedY = 0;
  uint16_t alignedWidth = 0;
  uint16_t alignedHeight = 0;
  uint32_t transferBytes = 0;
  uint16_t dirtyCells = 0;
  uint8_t dirtyRows = 0;
  uint8_t coalesced = 1;
  uint32_t firstSequence = 0;
  uint32_t lastSequence = 0;
  uint32_t freeHeap = 0;
  uint32_t minimumFreeHeap = 0;
};

Error decodeRequest(const uint8_t* payload, size_t length, Request& request);
void applyRequest(TerminalScreen& screen, const Request& request);
size_t encodeControlStatus(uint8_t* output, size_t capacity, Command command, Status status, Error error);
size_t encodeRefreshEvent(uint8_t* output, size_t capacity, const RefreshEvent& event);

}  // namespace knietty::diagnostics
