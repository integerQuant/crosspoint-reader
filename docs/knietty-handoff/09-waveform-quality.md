# Milestone 09 — Adaptive waveform quality

## Objective

Find the best measured interactive speed/contrast/ghosting tradeoff after
scheduler and baseline overhead are understood. Safe 20 MHz remains the release
control until a replacement clearly wins.

## Source anchors

- Adaptive build flags: [`platformio.ini`](../../platformio.ini#L182)
- SSD1677 refresh/profile selection:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L275)
- Custom LUT loading/timing:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L690)
- Terminal settle scheduling:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L642)
- Cursor rendering: [`src/terminal/TerminalFont.cpp`](../../src/terminal/TerminalFont.cpp#L41)

Re-read the SSD1677 waveform table encoding and voltage mapping from the retained
data sheet. Do not copy timings or voltages from a different panel/controller.

## Controlled variants

Change one dimension per build and encode it in the artifact name/telemetry:

- safe stock partial at 20 MHz;
- current one-frame adaptive at 40 MHz;
- twenty-frame adaptive at 20 MHz, targeting about 100 ms of directional drive;
- only after that comparison, shorter and longer 20 MHz durations that bracket
  the best observed quality/latency point.

Start from the controller's existing X4 analog settings. Do not alter source/gate
voltages in the first comparison. SPI overclock affects transfer only; waveform
frames affect particle drive. Keep those facts separate in analysis. The nominal
duration is not a measurement: use diagnostic BUSY time as the actual result.

Prefer more drive only for changed pixels and only in the transition direction
that needs it. Replace the inverse block cursor with an underline before judging
ghosting: toggling a large inverse cell creates avoidable high-frequency residue.
Quiet-time settling should cover only cells/pixels changed during the burst, not
blindly repaint the entire terminal.

## Measurement

Run baseline-v1 `latency`, `cadence`, and opt-in `ghosting` without changing
suite order or environmental metadata. Report:

- BUSY and total timing for each waveform;
- black-to-white and white-to-black separately;
- first-frame contrast and residue after 10/25/50/100 changes;
- quality after idle settle and after clean;
- queue/coalescing to ensure scheduler differences are not credited to LUTs;
- subjective reading notes plus fixed-camera images when available.

Normalize photographs for lighting/exposure only if the raw originals are kept.
Do not convert subjective quality into invented precision.

## Safety gate

All variants use volatile LUT/register writes only. The first quality experiment
uses the SSD1677's specified 20 MHz maximum write clock. Restore `PanelDefault`
and perform the established clean on exit. Cap test activations and stop on
stuck BUSY, unexpected flashing, severe persistent residue, or thermal/power
anomaly. Never issue OTP commands.

## Complete when

One profile is selected from reproducible speed and legibility evidence, or safe
20 MHz remains the winner. Record rejected variants and reasons so future agents
do not repeat them under new names.
