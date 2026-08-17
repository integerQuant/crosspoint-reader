# Milestone 07 — SSD1677 volatile Mode 2 RAM ping-pong

## Status

Software candidate implemented from parent `edf80251` with FreeInk experiment
`8ff8d51`. The separately named `knietty_mode2_pingpong` environment retains
W100 Sustain1, no automatic settle, 20 MHz SPI, TLS, and the current terminal
layout. It is compile-time gated, runtime-limited to `Board::XteinkX4`, and
advertises `ram_ping_pong: true` through session metadata. Six pure state-model
tests cover unavailable hardware, required seeding, odd/even full/window
activations, clean/reset, abort/power loss, and invalidation. The 49-test Rust
matrix and 173 native tests pass; the firmware builds. Physical behavior is not
yet claimed.

The driver seeds both complete RAMs through the existing absolute path before
writing volatile R37h. While active it writes only the new target through 0x24,
lets the controller exchange bank roles after the Mode-2 activation, and omits
the manual post-BUSY BW/RED rewrite. Any HALF/FULL, dark-background, settle,
async, dual-buffer, grayscale, turn-off, reset, or sleep transition leaves the
experiment through controller reset plus an absolute seed. The implementation
never references or sends Program OTP Selection command 0x36.

## Objective

Determine whether SSD1677 Display Mode 2 can eliminate the measured post-BUSY
BW/RED baseline rewrite without stale pixels or polarity errors.

The product target is not maximum refresh rate. Preserve the currently accepted
interactive cadence and treat any recovered `READY - PRESENTED` time as budget
for better contrast and lower ghosting in Milestone 09. Mode 2 must first pass
as an isolated bookkeeping optimization; do not alter the waveform in this
experiment or credit it with an optical improvement it cannot produce directly.

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

Create a separately named 20 MHz experimental environment. On entry:

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

The first candidate intentionally exercises controller window semantics instead
of adding a speculative host-side copy. This is the highest-risk part of the
gate: if an earlier cell or row reappears when a disjoint region changes, stop
the suite immediately. That result rejects zero-copy window ping-pong and the
next candidate must either synchronize the inactive window bank or restrict
Mode 2 to complete-frame transfers.

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

First-run order for the current candidate:

1. Confirm the timing page says `W100 sustain / Mode 2 ping-pong`.
2. Run only `smoke` first and watch every transition. In particular, the first
   cell must not return during the cursor update, and the first dirty row must
   not return during the disjoint-row update.
3. Confirm the JSONL session record contains `"ram_ping_pong":true` and
   interactive READY records report `baseline_us: 0` after initial seeding.
4. Confirm checker/full, invert-and-return, final clean, exit, and normal
   sleep/wake are correct.
5. Only after that pass, run latency, cadence, and burst, followed by ordinary
   typing and btop. Stop at the first stale-region, odd/even, BUSY, or recovery
   failure.

## Complete when

Either Mode 2 has repeatable visual correctness and a measured readiness gain,
without making the current waveform's readability or ghosting worse, or it is
rejected with a reproducible failure record. A negative result is a valid
completed milestone; do not retain speculative driver state. A successful
result provides a measured time budget for later waveform-quality work rather
than automatically becoming a faster terminal profile.
