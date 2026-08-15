# Milestone 08 — Async window pipeline and latest-frame-wins

## Objective

Receive and parse while the panel is BUSY, prepare one bounded pending update,
and launch it immediately when the driver becomes ready. Avoid FIFO buildup of
obsolete terminal frames.

## Source anchors

- Terminal RX and dirty scheduling:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L180)
- Current blocking window refresh:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L642)
- Activity's two fixed screen models:
  [`src/activities/terminal/TerminalActivity.h`](../../src/activities/terminal/TerminalActivity.h#L63)
- HAL async API: [`lib/hal/HalDisplay.h`](../../lib/hal/HalDisplay.h#L61)
- Facade async state:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp#L514)
- SSD1677 start/finish:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L411)

## Scheduler contract

There may be one displayed/in-flight snapshot and one newest pending snapshot.
While BUSY:

- network RX and parsing continue;
- new mutations merge into the pending dirty region;
- superseded intermediate presentation requests are coalesced, not queued;
- sequence accounting records everything represented by the pending image;
- the framebuffer/read sources required by the active transfer remain immutable.

When `PRESENTED` occurs, perform only the driver work proven necessary by
Milestone 07, then launch the newest pending state. A continuous stream must not
starve input, exit controls, diagnostics events, idle settle, or mandatory clean.

Do not allocate a second 48 KiB E Ink framebuffer on ESP32-C3. Reuse the two
bounded terminal models and a fixed/capped window staging buffer. Any window
above the cap uses a documented full-frame path without hidden vector growth.

## Implementation order

1. Express scheduler state as a small enum with explicit invariants.
2. Split window refresh into start and finish at HAL, facade, and SSD1677 layers.
3. Make `PRESENTED` observable at BUSY fall and `READY` after post-work.
4. Add latest-frame-wins merging and sequence-range telemetry.
5. Integrate quiet-time settle/clean as lower-priority jobs that are cancelled
   or narrowed when fresh terminal data arrives.
6. Add bounded fairness so a never-ending stream still handles Power/Back.

## Automated gate

Use a fake display with controlled BUSY transitions. Test input arriving before
activation, during BUSY, during post-work, and exactly at completion. Assert no
framebuffer mutation while a consumer requires it, no lost final state, correct
dirty unions, correct sequence ranges, and bounded pending storage.

## Hardware gate

Run baseline-v1 cadence unchanged. Compare host-to-`PRESENTED`,
host-to-`READY`, queue time, coalescing, and final screen checksum/contents.
Exercise fast typing, 200-line output, btop, Ctrl+C, disconnect, exit, inverted
mode, and subsequent Home sleep/wake. Evaluate safe 20 MHz first.

## Complete when

The final terminal state is always correct, memory is bounded, queue latency is
substantially reduced or the result is honestly negative, and safe display
quality/sleep/recovery behavior is unchanged.
