# Project: knietty

## Current milestone

The Wi-Fi proof of concept, 80 x 24 stabilization image, and Terminus turbo
image have run on the available China-locked X4. The user confirmed the Home
icon, normal sleep/wake, host disconnect on exit, UTF-8 box drawing, btop,
two-press exit, Terminus rendering, waiting tips, and timing page.

The current software checkpoint adds an eight-pixel terminal inset without
losing the 80th column, rotates CrossPoint's standard button hints onto the
physical right edge, makes Terminus the default, and splits refresh testing into
safe 20 MHz, adaptive 20 MHz, and adaptive 40 MHz builds. Adaptive mode uses the
one-frame waveform for immediate output, performs a forced-endpoint normal DU
settle after 250 ms of output silence, and schedules a HALF clean only after 80
interactive updates plus one second idle. Its measurements exclude waiting,
approval, diagnostics, first paint, and disconnect cleanup.

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
- Terminus 8 x 16 is the default flash-resident terminal font. Spleen remains
  an explicitly selectable 1,001-glyph profile covering Latin, Greek, Cyrillic,
  box/block drawing, Braille, and a small
  Powerline subset. An eight-pixel left bezel inset is recovered from eight cell
  gutters, so all 80 columns still end exactly at pixel 800.
- Terminus (937 glyphs), Spleen, and GNU Unifont (978 glyphs) have compiled
  profiles. `docs/terminal-font-gallery.html` renders the exact firmware bitmap
  bytes for all three choices; this is not a browser-font approximation.
- The 32-pixel header shows `knietty@host`, an exactly centered clock, and an
  aligned battery percentage/icon. The first waiting frame seeds a real battery
  reading. Approval hints use the configured logical Confirm and Back mapping
  and the terminal plane is cleared when approval ends.
- Waiting mode shows the hostname, address, host command, and control tips.
  Left/Right toggles connected-terminal queue/render/LUT/plane/BUSY/baseline
  timing, region size, range/average, and window/fallback/settle/clean counts.
- Confirm sends Enter, long Confirm sends Ctrl+C, Back sends Escape, long Back
  toggles whole-screen polarity, and arrows send VT100 cursor sequences. Power
  requires a second press within three seconds to leave Terminal.
- Rendering snapshots the model under a short lock, redraws only dirty spans,
  and uses the X4 differential window path for regions requiring at most 8 KiB
  of temporary transfer memory. Large/unsupported updates fall back to the
  resident full framebuffer. Adaptive refresh keeps a one-phase custom SSD1677
  LUT resident during output bursts, bulk-uploads its 105-byte table, then uses
  a bounded forced-target DU settle. The default panel profile is restored
  before returning to CrossPoint.
- The host creates an isolated PTY session/process group. Local Ctrl+C is
  written to that PTY; Ctrl+\\ exits the bridge even during retry waits. An
  established disconnect exits by default, while `--reconnect` enables daemon
  rediscovery. systemd and launchd templates select the latter.
- tmux remains the preferred child command when installed, preserving the
  session across bridge/device reconnects; `$SHELL` is the fallback.

## Known failures

- The physically tested Terminus turbo image still needs more than its
  four-pixel left inset, and its landscape hints are in the wrong place. The
  new eight-pixel inset and rotated right-edge hints are software-tested only.
- The one-frame turbo waveform has poor contrast and excessive ghosting, and
  improved perceived speed only modestly. The adaptive settle, LUT residency,
  bulk upload, and 20/40 MHz A/B profiles have not yet been tested on hardware.
- The new font is deliberately bounded and is not a full Nerd Font. Applications
  that require sixel, emoji, combining-cell shaping, or unimplemented xterm CSI
  behavior will still degrade.
- Linux behavior has not been tested. macOS testing alone does not establish
  Linux parity.
- The Wi-Fi protocol is unencrypted and unauthenticated beyond physical
  approval. Use it only on a trusted LAN.
- Directed broadcast can be filtered by guest Wi-Fi/client isolation; explicit
  `--host` is the fallback.
- 30 Hz and 60 Hz are not feasible claims for this controller path. The measured
  turbo BUSY waveform alone is 226.5 ms. Raw-panel high-refresh projects use
  per-pixel waveform timers, concurrent update regions, and early cancellation
  in FPGA/parallel-panel hardware that the SSD1677 command interface does not
  expose.
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
  The SSD1677 additionally records LUT upload, initial plane transfer, and
  post-waveform baseline synchronization. Terminal adds queue and render time,
  freezes the connected-session snapshot while showing diagnostics, and excludes
  non-terminal frames from its averages.
- The SSD1677 retains a custom LUT in controller RAM until an OTP/default
  activation, reset, sleep, or profile transition invalidates it. Adaptive burst
  updates therefore avoid re-uploading 105 LUT bytes one SPI transaction at a
  time; the first upload is now one bulk transaction.
- Modos/Caster-style 60 Hz work drives raw panel source/gate buses with FPGA and
  per-pixel state. On the X4, MASTER_ACTIVATION starts one global SSD1677
  waveform and BUSY prevents the next activation, so those algorithms cannot be
  transplanted through software alone. The realistic optimization space is
  bounded transfer, shorter/better waveforms, coalescing, and perceived-latency
  staging.
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
- The subsequent Terminus turbo artifact also flashed successfully. Terminus is
  preferred, waiting tips and diagnostics render, btop works, and Turbo is
  visibly faster, but the tips need rotation to the right edge, the left inset
  needs another four pixels, and the waveform has excessive ghosting and weak
  contrast.
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

$HOME/.platformio/penv/bin/pio run -e knietty_safe
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_oc

python3 scripts/generate_terminal_font_gallery.py
```

Formatting passes with clang-format 21.1.8 installed in the local uv-managed
`.venv`. The host suite passes 24/24 and the native suite passes 149/149. Safe,
adaptive 20 MHz, and adaptive 40 MHz firmware environments build. Safe reports
RAM 54,236 / 327,680 bytes and flash 5,641,427 / 6,553,600 bytes; adaptive
reports RAM 54,260 bytes and flash 5,642,317 bytes. These are linker figures,
not runtime heap measurements.

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

On the physically tested Terminus turbo image, after 50 displayed updates, the
user recorded: last 526.4 ms, waveform/BUSY 226.5 ms, transfer 164.7 ms, render
134.9 ms, average 576 ms, minimum 459 ms, and maximum 1975 ms. Those values came
from the first metrics implementation, which mixed waiting, approval,
diagnostics, first-frame, and disconnect/clean paints with terminal updates. In
particular, the 1975 ms maximum is consistent with a HALF clean and is not a
valid interactive maximum. The new metrics separate and exclude those paths;
no new safe/adaptive hardware measurements exist yet.

The observed 226.5 ms BUSY interval establishes a physical upper bound of about
4.4 completed global activations per second for that waveform on this unit,
before rendering and SPI work. The 40 MHz variant can only reduce transfer time;
it cannot halve the panel BUSY interval.

## Last known-good commit

- `60c30d06` is the latest physically booted terminal checkpoint. Its sleep/wake,
  icon, btop/glyph, exit, and host-disconnect behavior are known good; its
  remaining UI/latency observations are recorded above.
- `b80046b3` documents the physically tested Terminus turbo artifact and points
  to FreeInk commit `72ff720`.
- Official CrossPoint 1.5.0 is the physically tested recovery firmware.
- `33f07db7` is the built unmodified upstream baseline.

## Next concrete step

Flash the new safe artifact through the already-proven SD UI, then test in this
order before trying either experimental image:

1. Before opening Terminal, sleep and wake once from Home.
2. Open Terminal and confirm the control hints are rotated on the physical right
   edge. Start the host, verify mapped Accept/Deny hints, accept, and ensure the
   hints disappear.
3. At the prompt, verify the opening `(` is intact with the eight-pixel inset and
   that an 80-character line still fits. Run `printf 'line%03d\\n' {1..200}` and
   record every timing line, including queue, plane, LUT, baseline, region,
   fallback, settle, and clean counts.
   Also report contrast, ghosting, and whether black/white flashes remain.
4. Verify Ctrl+C interrupts a foreground PTY program while the bridge remains,
   Ctrl+\\ exits the bridge, and leaving knietty causes one clean disconnect.
5. Exit Terminal with the two-press Power action, then sleep and wake again from
   Home. If safe passes, repeat the same workload with adaptive 20 MHz. Test the
   40 MHz image last and immediately recover if corruption, unstable refreshes,
   or wake regressions appear.

BLE keyboard work starts only after those checks pass.
