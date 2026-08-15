# Project: knietty

## Current milestone

The Wi-Fi proof of concept and the `60c30d06` 80 x 24 stabilization image have
both run on the available China-locked X4. The user confirmed that the corrected
Home icon, normal sleep/wake, host disconnect on exit, UTF-8 box drawing, btop,
and two-press exit now work. The current milestone is a maximum-speed display
experiment plus the remaining terminal UI polish.

Checkpoint `c946d9ed` adds a four-pixel terminal inset without losing the 80th
column, restores mapped Accept/Deny hints, adds a waiting/control screen and
on-device timing diagnostics, removes periodic/inversion cleans, and selects an
experimental one-frame SSD1677 waveform only while Terminal is active. It also
adds exact-bitmap previews and build profiles for Spleen, Terminus, and GNU
Unifont. This checkpoint is software-tested but not yet flashed.

BLE keyboard work remains deferred until this Wi-Fi/display checkpoint is
stable on hardware.

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
  Powerline subset. A four-pixel left bezel inset is recovered from four cell
  gutters, so all 80 columns still end exactly at pixel 800.
- Terminus (937 glyphs) and GNU Unifont (978 glyphs) are optional compiled
  profiles. `docs/terminal-font-gallery.html` renders the exact firmware bitmap
  bytes for all three choices; this is not a browser-font approximation.
- The 32-pixel header shows `knietty@host`, an exactly centered clock, and an
  aligned battery percentage/icon. The first waiting frame seeds a real battery
  reading. Approval hints use the configured logical Confirm and Back mapping
  and the terminal plane is cleared when approval ends.
- Waiting mode shows the hostname, address, host command, and control tips.
  Left/Right toggles measured refresh totals, waveform wait, transfer/render
  time, range/average, and window/fallback/clean counts.
- Confirm sends Enter, long Confirm sends Ctrl+C, Back sends Escape, long Back
  toggles whole-screen polarity, and arrows send VT100 cursor sequences. Power
  requires a second press within three seconds to leave Terminal.
- Rendering snapshots the model under a short lock, redraws only dirty spans,
  and uses the X4 differential window path for regions requiring at most 8 KiB
  of temporary transfer memory. Large/unsupported updates fall back to the
  resident full framebuffer. Terminal Turbo uses a one-phase custom SSD1677 LUT;
  unchanged pixels idle, fixed periodic cleans are disabled, and the default
  panel profile is restored before returning to CrossPoint.
- The host creates an isolated PTY session/process group. Local Ctrl+C is
  written to that PTY; Ctrl+\\ exits the bridge even during retry waits. An
  established disconnect exits by default, while `--reconnect` enables daemon
  rediscovery. systemd and launchd templates select the latter.
- tmux remains the preferred child command when installed, preserving the
  session across bridge/device reconnects; `$SHELL` is the fallback.

## Known failures

- The installed `60c30d06` image still clips the first terminal character at
  the left bezel. It also lost the visible Accept/Deny button legend even though
  physical approval itself still works. `c946d9ed` addresses both but is not yet
  hardware-verified.
- On `60c30d06`, later/burst output still feels roughly 500 ms behind and the
  panel occasionally flashes black/white. The new image removes the fixed
  50-update HALF clean and the HALF refresh previously forced by inversion, then
  substitutes an experimental one-frame terminal waveform. Its contrast,
  ghosting, and actual latency are unknown until the device diagnostics are read.
- The new font is deliberately bounded and is not a full Nerd Font. Applications
  that require sixel, emoji, combining-cell shaping, or unimplemented xterm CSI
  behavior will still degrade.
- `c946d9ed` waiting tips, approval legend, four-pixel inset, first-frame battery,
  diagnostics, alternate fonts, and turbo waveform have not been observed on
  hardware.
- Linux behavior has not been tested. macOS testing alone does not establish
  Linux parity.
- The Wi-Fi protocol is unencrypted and unauthenticated beyond physical
  approval. Use it only on a trusted LAN.
- Directed broadcast can be filtered by guest Wi-Fi/client isolation; explicit
  `--host` is the fallback.
- 30 Hz and 60 Hz are not current claims. The turbo LUT requests one drive frame,
  but the complete controller/panel update includes LUT upload, RAM transfer,
  power state, gate scan, and BUSY handling. No numeric turbo timing has been
  measured.
- The default native CMake invocation on this development Mac does not find the
  Command Line Tools SDK libc++ headers. The explicit SDK include flag in Build
  commands is the tested workaround.

## Architecture findings

- Upstream baseline is `develop` at `33f07db7`; source version is CrossPoint
  1.5.0. Toolchain: pioarduino PlatformIO Core 6.1.19,
  platform-espressif32 55.3.37, Arduino-ESP32 3.3.7, ESP-IDF 5.5.2, and RISC-V
  GCC 14.2.0.
- The plain X4 board profile is 800 x 480 with an SSD1677 and one 48,000-byte
  1-bpp framebuffer. This is a source finding; the controller identity of the
  available physical unit has not been independently read back.
- `HalDisplay::FAST_REFRESH` maps to the SSD1677 fast waveform. Source comments
  document roughly 500 ms for the stock path, roughly 77 ms for the opt-in X4
  fast-DU shortcut, HALF at 1720 ms, and FULL around 1800 ms. These are source
  values, not measurements from knietty.
- FreeInk already implements byte-aligned SSD1677 rectangular differential
  refresh. knietty exposes it through `HalDisplay` and `GfxRenderer`, transforms
  logical orientation to panel memory, and caps its transient vector at 8 KiB.
  X3, factory-LUT, fading-fix, and larger regions use full-buffer fallback.
- The SSD1677 data sheet maps RED/BW RAM pairs 00/01/10/11 to LUT0/1/2/3.
  Terminal Turbo therefore idles unchanged black/white, drives black-to-white
  with one VSL phase, and white-to-black with one VSH1 phase. The profile is an
  explicit driver capability: unsupported controllers report PanelDefault, and
  exiting Terminal restores PanelDefault. FULL/HALF paths remain unchanged.
- Blocking display calls now record total, BUSY/waveform, and non-waveform time.
  Terminal adds render time and exposes the results while waiting; this avoids
  treating configured batch intervals or source comments as measurements.
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
- The subsequent `60c30d06` SD flash succeeded. On that image the Home icon is
  correct, normal sleep/wake works, exiting Terminal cleanly disconnects the
  default host bridge, btop and box drawing work, and physical connection
  approval still accepts/denies correctly. Remaining observations are the
  invisible approval legend, left-edge clipping, occasional black/white flashes,
  and roughly 500 ms perceived burst cadence.
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
- The proof-of-concept, `0217ada8`, and `60c30d06` bridges were physically
  exercised through discovery, approval, connection, and interactive output.
  `60c30d06` confirmed the default bridge exits on a clean device disconnect.
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
.venv/bin/pio run -e knietty_terminus
.venv/bin/pio run -e knietty_unifont

python3 scripts/generate_terminal_font_gallery.py
```

Formatting passes with clang-format 21.1.8 installed in the local uv-managed
`.venv`. The host suite passes 24/24 and the native suite passes 149/149. All
three font firmware environments build. The default checkpoint reports RAM
54,228 / 327,680 bytes (16.5%) and flash 5,642,215 / 6,553,600 bytes (86.1%).
These are linker figures, not runtime heap measurements.

The earliest physically installed artifact retained for comparison is:

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

`60c30d06` is now physically tested as described above. The experimental turbo
artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-c946d9ed-80x24-turbo.bin
```

It was clean-built from `c946d9ed`, is 5,656,064 bytes, embeds version
`1.5.0-dev-feature/knietty-terminal-c946d9ed`, and has SHA-256
`ad73dd7a8def134426b1872a4e3d2c304540af8760665bc4584d600d78032062`.
The artifact uses Spleen; the Terminus and GNU Unifont profiles were
compile-validated but were not copied as release artifacts.

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

No numeric hardware measurements yet. On `0217ada8` and `60c30d06`, the user
observed a fast first visual response, later/burst state lag that felt around
500 ms, and quick pixel motion once a waveform began. The 8/20 ms batching
interval, 64 KiB/s host pacing, 8 KiB window-memory cap, and one-frame turbo LUT
are configuration/source facts, not observed timing results. `c946d9ed` adds the
on-device measurement needed for the next test.

## Last known-good commit

- `60c30d06` is the latest physically booted terminal checkpoint. Its sleep/wake,
  icon, btop/glyph, exit, and host-disconnect behavior are known good; its
  remaining UI/latency observations are recorded above.
- `c946d9ed` is the software-tested turbo/UI/font checkpoint. It points to
  FreeInk commit `72ff720` and is not yet physically validated.
- Official CrossPoint 1.5.0 is the physically tested recovery firmware.
- `33f07db7` is the built unmodified upstream baseline.

## Next concrete step

Flash `knietty-c946d9ed-80x24-turbo.bin` through the already-proven SD UI,
then test in this order:

1. Before opening Terminal, sleep and wake once from Home.
2. Open Terminal and confirm the waiting tips, actual battery value, hostname/IP,
   and Left/Right timing page. Start the host and verify the mapped Accept/Deny
   footer is visible, then accept and ensure it disappears.
3. At the prompt, verify the opening `(` is intact with a small left inset and
   that an 80-character line still fits. Run `printf 'line%03d\\n' {1..200}` and
   record the timing page's last/average/min/max and waveform/transfer values.
   Also report contrast, ghosting, and whether black/white flashes remain.
4. Verify Ctrl+C interrupts a foreground PTY program while the bridge remains,
   Ctrl+\\ exits the bridge, and leaving knietty causes one clean disconnect.
5. Exit Terminal with the two-press Power action, then sleep and wake again from
   Home. Stop and recover to official 1.5.0 if the turbo waveform is unreadable
   or wake regresses.

BLE keyboard work starts only after those checks pass.
