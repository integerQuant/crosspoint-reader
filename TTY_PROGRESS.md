# Project: knietty

## Current milestone

The Wi-Fi proof of concept and its 80 x 24 follow-up have both run on the
available China-locked X4. The current milestone is stabilization: restore
reliable sleep/wake outside Terminal, remove terminal layout artifacts, reduce
refresh scope, improve glyph coverage, and make the host bridge terminate
predictably.

Three software-tested checkpoints now address the latest hardware report:

- `ca62ed99` restores the stable CrossPoint 1.5.0 sleep/wake path for knietty;
- `2f3cd04d` adds full-width Spleen rendering, UTF-8, dirty spans, bounded X4
  window refresh, prompt cleanup, and the corrected Home icon;
- `24f32e13` isolates PTY signals, exits cleanly after disconnect by default,
  supports explicit daemon reconnection, and rate-limits retry logs.

None of those three checkpoints has been tested on the physical X4 yet. BLE
keyboard work remains deferred until this Wi-Fi/display checkpoint is stable.

## Working features

- The Terminal activity uses CrossPoint's saved-network selector, advertises
  `_knietty._tcp.local`, answers bounded UDP discovery, requires physical
  approval, and carries a raw bidirectional stream over TCP with TCP_NODELAY.
- Protocol v2 negotiates an 80 x 24 PTY and transfers host epoch/time-zone data
  for the header clock. The host falls back to v1 for the 50 x 22 proof of
  concept.
- The fixed terminal model supports delayed VT wrapping, scrolling, dirty
  column spans, cursor state, basic CSI/SGR, and incremental UTF-8. Invalid or
  non-BMP input consumes one replacement cell.
- Spleen 8 x 16 supplies 1,001 glyphs from a flash-resident generated table,
  including Latin, Greek, Cyrillic, box/block drawing, Braille, and a small
  Powerline subset. Cells are 10 x 18; 80 columns occupy all 800 pixels with a
  one-pixel guard inside each cell.
- The 32-pixel header shows `knietty@host`, an exactly centered clock, and an
  aligned battery percentage/icon. Approval hints use the configured logical
  Confirm and Back mapping and the terminal plane is cleared when approval
  ends.
- Confirm sends Enter, long Confirm sends Ctrl+C, Back sends Escape, long Back
  toggles whole-screen polarity, and arrows send VT100 cursor sequences. Power
  requires a second press within three seconds to leave Terminal.
- Rendering snapshots the model under a short lock, redraws only dirty spans,
  and uses the X4 differential window path for regions requiring at most 8 KiB
  of temporary transfer memory. Large/unsupported updates fall back to the
  resident full framebuffer. Entry and every 50 fast updates use HALF clean.
- The host creates an isolated PTY session/process group. Local Ctrl+C is
  written to that PTY; Ctrl+\\ exits the bridge even during retry waits. An
  established disconnect exits by default, while `--reconnect` enables daemon
  rediscovery. systemd and launchd templates select the latter.
- tmux remains the preferred child command when installed, preserving the
  session across bridge/device reconnects; `$SHELL` is the fallback.

## Known failures

- The physically installed `0217ada8` build breaks wake from sleep even after
  leaving Terminal and entering sleep elsewhere in CrossPoint. A soft reset is
  required. `ca62ed99` disables the development branch's new light-sleep hooks
  in the knietty environment and restores the 1.5.0 quick-resume decision path,
  but that fix is not hardware-verified.
- On `0217ada8`, the Home icon is rotated clockwise, the approval hints remain
  after connection, the terminal has excess left margin, the font is narrow,
  unsupported glyphs appear as `?`, the battery can show 0 while waiting, and
  burst display state feels roughly 500 ms behind. The new checkpoint addresses
  each code-side cause except that actual waveform latency still requires
  measurement.
- The new font is deliberately bounded and is not a full Nerd Font. Applications
  that require sixel, emoji, combining-cell shaping, or unimplemented xterm CSI
  behavior will still degrade.
- Window refresh, corrected icon orientation, prompt cleanup, new glyphs,
  inversion, clock/battery placement, and sleep/wake have not yet been observed
  on the new image.
- Linux behavior has not been tested. macOS testing alone does not establish
  Linux parity.
- The Wi-Fi protocol is unencrypted and unauthenticated beyond physical
  approval. Use it only on a trusted LAN.
- Directed broadcast can be filtered by guest Wi-Fi/client isolation; explicit
  `--host` is the fallback.
- 30 Hz and 60 Hz are not current claims. The source-documented X4 fast-DU path
  is roughly 77 ms before application overhead, so its theoretical ceiling is
  below 13 Hz. No numeric knietty panel timing has been measured.
- The default native CMake invocation on this development Mac does not find the
  Command Line Tools SDK libc++ headers. The explicit SDK include flag in Build
  commands is the tested workaround.

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
- FreeInk already implements byte-aligned SSD1677 rectangular differential
  refresh. knietty exposes it through `HalDisplay` and `GfxRenderer`, transforms
  logical orientation to panel memory, and caps its transient vector at 8 KiB.
  X3, factory-LUT, fading-fix, and larger regions use full-buffer fallback.
- Bundled CrossPoint UI fonts are proportional. The generated Spleen table is
  18 bytes per glyph in flash; switching to Unicode cells raises each terminal
  model from about 3.8 KiB to 7.7 KiB. Terminal owns two bounded models so RX can
  continue during display work. Linker-reported static RAM did not increase.
- X3/X4 exposes logical Back, Confirm, Left, Right, Up, Down, and Power.
  Terminal uses `MappedInputManager`, not physical GPIO IDs.
- CrossPoint network activities stop mDNS, disconnect Wi-Fi, and use
  `silentRestart()` when leaving. Terminal follows that lifecycle.
- The knietty build omits serial logging. Native USB is Arduino-ESP32 `HWCDC`,
  but no CDC node appeared on the tested locked unit, so Wi-Fi is primary.
- `KNIETTY_STABLE_POWER` is isolated to the feature environment: it disables
  the branch's experimental BUSY-slice/main-loop light-sleep behavior, closes
  the CDC object before deep sleep, and retains 1.5.0 quick-resume semantics.

## Hardware observations

- Device: China-locked XTEINK X4, initially running CrossPoint 1.4.1 installed
  through the web unlock tool. The exact 1.4.1 application binary is retained.
- The proof-of-concept knietty image booted and its Wi-Fi terminal was approved,
  discovered, connected, and used interactively. The user signed it off.
- LAN discovery found `knietty-9e54a0` at `192.168.0.251:29380`.
- The proof-of-concept's SD updater failed at displayed 1% for both a polished
  knietty image and the retained 1.4.1 image. The active image stayed bootable.
- CrossPoint's normal network OTA then restored official base 1.5.0 without the
  Unlocker. From that clean base, the SD-menu update to
  `knietty-0217ada8-80x24-sd-safe.bin` completed successfully and booted. This
  establishes normal OTA as recovery and the current SD application path as a
  viable custom update path.
- The installed `0217ada8` terminal confirmed 80 x 24 geometry, Wi-Fi operation,
  connection approval, and two-press exit. It also produced the UI, glyph,
  disconnect, latency, and sleep failures listed above.
- No USB CDC `/dev/cu.usbmodem*` node was observed on the connected Mac.
- No partition table, bootloader, secure-boot, or eFuse changes were made.

## Linux host observations

Not tested. The implementation uses POSIX PTY/select/socket APIs and includes a
user systemd template with `--reconnect`, but that is source-level portability
only.

## macOS host observations

- Development host: Darwin/macOS, shell `/bin/zsh`.
- The host suite passes 24/24 tests. Coverage includes discovery and protocol
  parsing, portable PTY sizing/environment, raw local terminal restoration,
  Ctrl+\\ during retry, log rate limiting, and a live subprocess assertion that
  Ctrl+C signals only the PTY child process group.
- The proof-of-concept and `0217ada8` bridge were physically exercised through
  discovery, approval, connection, and interactive output. The new disconnect
  behavior has not yet been tested against the X4.
- LaunchAgent behavior has not been tested as an installed user agent.

## Build commands

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader

env PATH="$PWD/.venv/bin:$PATH" ./bin/clang-format-fix -g

uv run --project host --no-sync \
  python -m unittest discover -s host/tests -v

native_test_dir=$(mktemp -d /tmp/knietty-tests.XXXXXX)
cmake -S test -B "$native_test_dir" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-isystem /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk/usr/include/c++/v1'
cmake --build "$native_test_dir" -j4
ctest --test-dir "$native_test_dir" --output-on-failure

.venv/bin/pio run -e knietty
```

Formatting passes with clang-format 21.1.8 installed in the local uv-managed
`.venv`. The host suite passes 24/24, the native suite passes 147/147, and the
knietty firmware build succeeds. PlatformIO reports RAM 54,212 / 327,680 bytes
(16.5%) and flash 5,637,065 / 6,553,600 bytes (86.0%). These are linker figures,
not runtime heap measurements.

The physically installed prior artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-0217ada8-80x24-sd-safe.bin
```

It is 5,634,208 bytes with SHA-256
`fab38a4168101b139cfe37954de1425ae9fd9b737181ae07e54b6451be2aa687`.
The stabilization artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-60c30d06-80x24-windowed.bin
```

It was clean-built from `60c30d06`, is 5,650,912 bytes, embeds version
`1.5.0-dev-feature/knietty-terminal-60c30d06`, and has SHA-256
`2f8b5367669a5a9ad6fe1bf4313379839c90fac53c7d066a72a29ad1335c5647`.

## Flash/update commands

For this locked unit, do not use PlatformIO upload or esptool. Copy only the
application `.bin` artifact to the SD card and select it through CrossPoint's
normal in-application firmware update UI. The user has physically completed
that flow once from official 1.5.0 to the SD-safe knietty image.

Do not alter partitions, bootloader, secure-boot state, or eFuses.

## Recovery procedure

1. Keep the exact CrossPoint 1.4.1 binary and every known bootable knietty image
   outside the build directory.
2. If a new application update fails, do not retry destructive low-level tools;
   the current slot should remain selected.
3. Use CrossPoint's normal network OTA to restore official firmware. This route
   physically restored official 1.5.0 without the Unlocker.
4. Do not alter otadata manually, partitions, bootloader, secure-boot state, or
   eFuses.

## Performance measurements

No numeric hardware measurements yet. On `0217ada8`, the user observed a fast
first visual response, later/burst state lag that felt around 500 ms, and quick
pixel motion once a waveform began. The 8/20 ms batching interval, 64 KiB/s host
pacing, 50-fast-refresh clean interval, 8 KiB window-memory cap, and driver
waveform comments are configuration/source facts, not observed timing results.

## Last known-good commit

- `0217ada8` is the latest physically booted terminal checkpoint. It has the
  breaking post-Terminal sleep/wake failure and the UI issues above.
- `60c30d06` contains the built/software-tested stabilization checkpoints and
  is the source of the new SD artifact; it is not yet physically validated.
- Official CrossPoint 1.5.0 is the physically tested recovery firmware.
- `33f07db7` is the built unmodified upstream baseline.

## Next concrete step

Flash `knietty-60c30d06-80x24-windowed.bin` through the already-proven SD UI,
then test in this order:

1. Before opening Terminal, sleep and wake once from Home.
2. Open Terminal, approve the host, then verify the icon, prompt cleanup, zero
   left margin without clipping, clock/battery alignment, UTF-8/box drawing, and
   80 x 24 geometry.
3. Run `printf 'line%03d\\n' {1..200}` and a full-width 80-character line plus
   CRLF. Observe window latency and ghosting; do not infer a refresh rate.
4. Verify Ctrl+C interrupts a foreground PTY program while the bridge remains,
   Ctrl+\\ exits the bridge, and leaving knietty causes one clean disconnect.
5. Exit Terminal with the two-press Power action, then sleep and wake again from
   Home. Stop and recover to official 1.5.0 if wake still fails.

BLE keyboard work starts only after those checks pass.
