# Project: knietty

## Current milestone

The Wi-Fi terminal proof of concept is signed off on the available China-locked
X4. The current checkpoint polishes terminal layout, input, exit safety, burst
handling, and refresh behavior. Its firmware build is ready for a physical
smoke test; BLE keyboards and windowed/waveform experiments remain deferred.

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
  but has not yet been flashed or exercised on the physical X4. The Power exit,
  inversion, 80 x 24 legibility, header, burst behavior, and removal of periodic
  black flashes therefore still require hardware confirmation.
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
(16.5%) and flash 5,619,739 / 6,553,600 bytes (85.8%). These are linker figures,
not runtime heap measurements.

## Flash/update commands

For this locked unit, do not use PlatformIO upload or esptool. The user has
successfully installed a prior knietty `firmware.bin` with the same web custom
image flow that accepts the retained 1.4.1 binary. Use only the generated
application `firmware.bin` through that already-proven web flow; partitions,
bootloader, and eFuses remain untouched.

## Recovery procedure

The known-good 1.4.1 application binary is retained and accepted by the web
installer, but restoration has not been exercised:

1. Keep the exact CrossPoint 1.4.1 binary and the currently working knietty
   image outside the build directory.
2. Do not alter the partition table, bootloader, secure-boot state, or eFuses.
3. If the polished build fails, use the same proven web custom-image flow to
   select the retained 1.4.1 binary, then record whether restoration completes.

## Performance measurements

No numeric hardware measurements yet. The user observed low latency on the
first input and noticeably higher latency for the next/burst updates on the
proof of concept. The 8/20 ms batching, 64 KiB/s pacing, 50-fast-refresh clean
interval, and driver-comment waveform durations are configuration/source facts,
not observed timing results.

## Last known-good commit

`6c3a2fa4` is the committed and physically validated Wi-Fi proof-of-concept.
The polished 80 x 24 checkpoint is pending its final commit and physical test.
`33f07db7` remains the built unmodified upstream baseline.

## Next concrete step

Install the polished `firmware.bin` through the same web custom-image flow,
open **knietty**, and run:

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader
uv run --project host --no-sync knietty --host auto --verbose
```

Verify approval labels, 80 x 24 layout, full-width CRLF, Ctrl+C, Ctrl+\\, a
burst such as `seq 1 200`, long-Back inversion, and the two-press Power exit.
Do not test sleep from inside knietty; exit first. Then measure refresh/input
latency and ghosting before starting BLE keyboard work.
