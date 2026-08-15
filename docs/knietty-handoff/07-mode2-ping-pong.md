# Milestone 07 — SSD1677 volatile Mode 2 RAM ping-pong

## Objective

Determine whether SSD1677 Display Mode 2 can eliminate the measured post-BUSY
BW/RED baseline rewrite without stale pixels or polarity errors.

## Source anchors

- Current full-frame baseline rewrite:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L506)
- Current window baseline rewrite:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L577)
- Driver state/profile fields:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.h`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.h#L139)
- Current timing facade:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp#L942)

Also re-read the retained SSD1677 data sheet section for volatile display-option
register `0x37`, F6, immediately before implementation. Do not infer behavior
from the register name alone.

## Experiment design

Create a separately named safe-20 experimental environment. On entry:

1. initialize the controller normally;
2. seed both complete RAM banks with the same known screen;
3. set only the documented volatile Mode 2 option;
4. run one update at a time, waiting for `PRESENTED` and `READY`;
5. track the expected bank role explicitly in driver state;
6. disable Mode 2 and perform an absolute clean before returning to CrossPoint.

Do not program waveform/display-option OTP. Do not combine this experiment with
40 MHz, custom waveform changes, gate MUX changes, or async scheduling.

Test full frame and byte-aligned windows separately. Alternate black/white,
single-pixel-cell, disjoint-row, scroll, checkerboard, and inverted patterns so
a wrong old/new bank becomes visually obvious. Force disconnect/abort after odd
and even activation counts.

## Diagnostics questions

- Does `baselineUs` become zero or nearly zero?
- Is `READY - PRESENTED` reduced by the former baseline duration?
- Does the controller swap after every activation, only selected modes, or not
  as expected?
- Do window and full-frame activation use the same bank semantics?
- What happens after clean/HALF, profile change, reset, sleep, and power-off?

## Automated gate

Model the bank-state transition table with unit tests before hardware. Test
entry seeding, odd/even updates, window/full transitions, abort, power-down,
clean, and restore. All unsupported controllers must stay on their existing path.

## Hardware gate

Flash safe control first and run diagnostics smoke. Flash only the labeled
Mode-2-safe experiment through the proven SD path. Abort at the first corruption,
stuck BUSY, refresh instability, or wake regression; recover through normal OTA.
Capture identical baseline-v1 suites before/after.

## Complete when

Either Mode 2 has repeatable visual correctness and a measured readiness gain,
or it is rejected with a reproducible failure record. A negative result is a
valid completed milestone; do not retain speculative driver state.
