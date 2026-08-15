# Project: knietty

## Current milestone

The Wi-Fi terminal proof of concept is signed off on the available China-locked
X4. The polished firmware passed software validation but its first SD-card
installation attempt failed during the flash write at the first displayed 1%.
The current work is isolating that update-path failure before another custom
image is attempted; BLE keyboards and windowed/waveform experiments remain
deferred.

## Working features

- CrossPoint's saved-network selector launches a Wi-Fi terminal that advertises
  `_knietty._tcp.local`, answers UDP discovery, requires physical approval, and
  carries a raw bidirectional terminal stream over TCP with TCP_NODELAY.
- Protocol v2 negotiates an 80 x 24 PTY and transfers host epoch/time-zone data
  for the X4 clock. The host automatically falls back to the v1 handshake for
  the original 50 x 22 proof-of-concept firmware.
- The 80 x 24 model uses 9 x 18 fixed cells, centered at x=40 on the 800 x 480
  landscape display. A delayed VT wrap fixes the extra blank line and stray
  prompt produced by an exactly full line followed by CRLF.
- The compact 32-pixel header shows `knietty@host`, a host-synchronized clock,
  battery percentage/icon, and connection/exit state. The Home entry has a
  dedicated terminal icon.
- The approval view maps `Deny` and `Accept` through `MappedInputManager`, so
  the hints follow the actual physical front-button mapping.
- Terminal mode prevents auto-sleep and owns the global Power action. Press
  Power once to show `Press Power again to exit`; press it again within three
  seconds to return through the normal Activity lifecycle.
- Short Back sends Escape. Long Back toggles a session-local whole-screen
  black/white polarity mode. Confirm sends Enter; long Confirm sends Ctrl+C;
  the direction buttons send VT100 arrows.
- Firmware parses TCP input into the live terminal model while an E Ink
  waveform is running. Rendering takes a short locked snapshot, so a burst is
  collapsed into the latest screen state instead of waiting byte-for-byte
  behind the previous panel refresh.
- Output batching remains 8 ms interactive / 20 ms maximum. Whole-buffer fast
  refresh remains in use; the four-second idle HALF refresh was removed, and a
  HALF clean is retained after 50 fast refreshes or an explicit polarity change.
- The host uses raw local terminal mode. Ctrl+C reaches the remote PTY; Ctrl+\\
  exits the bridge and restores the host terminal. Wi-Fi output pacing is now
  65,536 bytes/s.
- tmux remains the default when installed, preserving the PTY session through
  disconnect/reconnect. The implementation and templates are shared between
  Linux and macOS; USB remains a legacy option.

## Known failures

- The polished firmware in this checkpoint has built and passed software tests
  but its first physical SD-card installation failed after validation, at the
  first displayed 1%, with `Update failed` / `Firmware write failed`. The active
  proof-of-concept firmware still runs. The Power exit, inversion, 80 x 24
  legibility, header, burst behavior, and removal of periodic black flashes
  therefore remain untested.
- The previous proof-of-concept could enter sleep from knietty and then fail to
  wake without a soft reset. This checkpoint prevents both automatic and global
  Power-button sleep while Terminal owns the screen, but the mitigation is not
  yet hardware-verified.
- Linux behavior has not been tested. macOS testing alone does not establish
  Linux parity.
- The protocol is unencrypted and unauthenticated beyond physical approval. It
  is suitable only for a trusted LAN at this stage.
- Directed broadcast can be filtered by guest Wi-Fi/client-isolation rules;
  explicit `--host` is the fallback.
- 30 Hz and 60 Hz are not current claims. The source-documented X4 fast-DU path
  is roughly 77 ms before application overhead, so even its theoretical ceiling
  is below 13 Hz. Actual latency and ghosting still need measurement.
- `clang-format` 21 is absent on the development Mac. `git diff --check` passes.
- The local Command Line Tools install omits its SDK libc++ include directory
  from the default CMake compiler search path. Native test rebuilds require the
  explicit `CPLUS_INCLUDE_PATH` shown below.

## Architecture findings

- Upstream baseline is `develop` at `33f07db7`; source version is CrossPoint
  1.5.0. Toolchain: pioarduino PlatformIO Core 6.1.19,
  platform-espressif32 55.3.37, Arduino-ESP32 3.3.7, ESP-IDF 5.5.2, and RISC-V
  GCC 14.2.0.
- X4 geometry is 800 x 480, SSD1677, with one 48,000-byte 1-bpp framebuffer.
- `HalDisplay::FAST_REFRESH` maps to the SSD1677 fast waveform. Source comments
  document roughly 500 ms for the stock path, roughly 77 ms for the opt-in X4
  fast-DU shortcut, HALF at 1720 ms, and FULL around 1800 ms. These are source
  values, not measurements from knietty.
- `FreeInkDisplay::displayWindow()` and a byte-aligned SSD1677 implementation
  exist, but the method is not exposed through `HalDisplay`/`GfxRenderer` and
  the driver allocates temporary vectors. This checkpoint keeps whole-buffer
  updates; rectangular refresh is a later experiment.
- Bundled UI fonts are proportional. knietty uses its bounded bitmap ASCII
  font, now rendered 1x horizontally and 2x vertically.
- Physical X3/X4 inputs expose logical Back, Confirm, Left, Right, Up, Down, and
  Power. Terminal code uses `MappedInputManager`, not raw GPIO IDs.
- CrossPoint network activities stop mDNS, disconnect Wi-Fi, and use
  `silentRestart()` when leaving. Terminal follows that established lifecycle.
- Native USB is Arduino-ESP32 `HWCDC`; no CDC device node appeared on this unit.
  The knietty environment omits serial logging so terminal data stays clean.

## Hardware observations

- Device: China-locked XTEINK X4, previously running CrossPoint 1.4.1 installed
  through the web unlock tool. The exact 1.4.1 binary is retained and is
  accepted by the web installer.
- The proof-of-concept knietty image was flashed successfully, booted, and
  displayed its Home-menu entry.
- LAN discovery consistently found `knietty-9e54a0` at
  `192.168.0.251:29380`. Physical approval connected the host and terminal
  input/output worked; the user signed off the proof of concept.
- Qualitatively, the first input update was low latency while the second and
  burst updates lagged. Periodic HALF refresh caused a black flash, sleep failed
  to wake, the left edge clipped glyphs, and full-width/newline behavior lost a
  row and exposed a `%` prompt. This checkpoint addresses those causes in code.
- No USB CDC `/dev/cu.usbmodem*` node was observed on the connected Mac.
- On 2026-08-15 the polished 5,633,584-byte image passed the on-device firmware
  validation and confirmation screens, began the SD update, displayed 1%, then
  failed with the generic `Firmware write failed` message. CrossPoint maps SD
  read, partition erase/write, OTA-data, open, and allocation errors to that
  same message, so the exact failing operation is not observable from this
  build. At this image size, 1% is reached at roughly 57 KiB, immediately before
  the flasher's next 64 KiB erase boundary. The inactive slot may therefore be
  failing to erase/write, or the first progress repaint may be contending with
  the SD stream on the X4's shared display/SD SPI bus. These are source-based
  hypotheses, not confirmed hardware diagnoses.

## Linux host observations

Not tested. The code uses POSIX PTY/select/socket APIs and includes a user
systemd template, but that is source-level portability only.

## macOS host observations

- Development host: Darwin/macOS, shell `/bin/zsh`.
- The host suite passes 21/21 tests. Coverage includes device selection, UDP
  discovery parsing, v1/v2 response handling, PTY sizing/environment, raw local
  terminal restoration, Ctrl+C forwarding before Ctrl+\\ exit, and 80 x 24
  defaults.
- The proof-of-concept bridge was physically tested through discovery,
  approval, connection, and interactive terminal output. The new protocol-v2
  clock and polished control behavior have not yet been tested on hardware.
- LaunchAgent behavior has not been tested as an installed user agent.

## Build commands

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader

uv run --project host --no-sync \
  python -m unittest discover -s host/tests -q

env CPLUS_INCLUDE_PATH="$(xcrun --show-sdk-path)/usr/include/c++/v1" \
  cmake --build build/test -j
ctest --test-dir build/test --output-on-failure -j

.venv/bin/pio run -e knietty
```

The host suite passes 21/21 and the native suite passes 144/144. The current
knietty firmware build succeeds. PlatformIO reports RAM 54,228 / 327,680 bytes
(16.5%) and flash 5,620,359 / 6,553,600 bytes (85.8%). These are linker figures,
not runtime heap measurements.

The first polished build (the image that failed at 1% on the X4) is retained at:

```text
/Users/rodrigomtorres/git/knietty/knietty-bf069e31-80x24.bin
```

It is 5,633,584 bytes and has SHA-256
`922f78d9a30d1d3bb0e0a91f26ded5de37f5beee3600824d5350d7ab1fea69c5`.

The SD-hardened post-commit build is copied to:

```text
/Users/rodrigomtorres/git/knietty/knietty-0217ada8-80x24-sd-safe.bin
```

It is 5,634,208 bytes and has SHA-256
`fab38a4168101b139cfe37954de1425ae9fd9b737181ae07e54b6451be2aa687`.
The knietty SD updater in this image keeps the update screen static throughout
the raw write and appends the exact flasher result to future failure screens.
This hardening cannot affect the updater in the currently running
proof-of-concept and is not authorization to bypass the 1.4.1 A/B test.

## Flash/update commands

For this locked unit, do not use PlatformIO upload or esptool. The user
successfully installed the proof-of-concept through CrossPoint's SD-card
firmware update flow. The polished image's first attempt through that same flow
failed at 1%; do not retry it until the known-good-image A/B check below.
Partitions, bootloader, and eFuses remain untouched.

## Recovery procedure

The known-good 1.4.1 application binary is retained and accepted by the
on-device validator, but restoration has not been exercised:

1. Keep the exact CrossPoint 1.4.1 binary and the currently working knietty
   image outside the build directory.
2. Do not alter the partition table, bootloader, secure-boot state, or eFuses.
3. Use the current proof-of-concept's SD updater to select the exact retained
   1.4.1 binary as an A/B test. If it also fails at 1%, stop: the inactive slot
   or updater path is at fault. If it succeeds, boot 1.4.1 and use its already
   proven SD updater for the next knietty artifact.
4. A failed raw application write does not select the incomplete slot;
   `OtaBootSwitch` runs only after all bytes are written, so the current active
   proof-of-concept should remain bootable. Do not alter otadata manually.

## Performance measurements

No numeric hardware measurements yet. The user observed low latency on the
first input and noticeably higher latency for the next/burst updates on the
proof of concept. The 8/20 ms batching, 64 KiB/s pacing, 50-fast-refresh clean
interval, and driver-comment waveform durations are configuration/source facts,
not observed timing results.

## Last known-good commit

`0217ada8` is the built and software-tested SD-hardened 80 x 24 checkpoint.
`6c3a2fa4` remains the last physically validated Wi-Fi proof-of-concept until
the new artifact passes its X4 smoke test. `33f07db7` remains the built
unmodified upstream baseline.

## Next concrete step

With the X4 charged and externally powered, use the current proof-of-concept's
SD updater to install the exact retained CrossPoint 1.4.1 binary once. Do not
retry the polished image first. Record whether 1.4.1 also fails at 1% or writes
fully and boots. This separates an inactive-slot/updater failure from a
candidate-specific failure without selecting a partial image.

If 1.4.1 succeeds, install the next hardened knietty artifact from 1.4.1, open
**knietty**, and run:

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader
uv run --project host --no-sync knietty --host auto --verbose
```

Verify approval labels, 80 x 24 layout, full-width CRLF, Ctrl+C, Ctrl+\\, a
burst such as `seq 1 200`, long-Back inversion, and the two-press Power exit.
Do not test sleep from inside knietty; exit first. Then measure refresh/input
latency and ghosting before starting BLE keyboard work.
