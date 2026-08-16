# Project: knietty

## Project description

**knietty turns an XTEINK X4 running CrossPoint into a low-latency wireless
terminal for shell and tmux sessions.** It discovers a host over the local
network, renders a compact VT-style terminal optimized for E Ink, and relays
device input back to the host without replacing CrossPoint's reader experience.

Short description: **A wireless TTY for your E Ink reader.**

## Current milestone

The Wi-Fi proof of concept, 80 x 24 stabilization image, and Terminus turbo
image have run on the available China-locked X4. The user confirmed the Home
icon, normal sleep/wake, host disconnect on exit, UTF-8 box drawing, btop,
two-press exit, Terminus rendering, waiting tips, and timing page.

Milestone 01 is physically validated. Terminal now
temporarily disables the renderer's fading fix, preserves the incoming refresh
profile/orientation, and restores all of that state on exit without changing the
saved reader setting. A race-safe render gate also preserves one coalesced
follow-up paint when network state changes during an in-flight E Ink refresh;
the user confirmed the formerly invisible host approval prompt now renders.

Milestones 02 and 03 are physically validated on the available X4. Protocol v3
carried the normal terminal, forced v2 remained compatible, and the distinct
diagnostics approval, denial, abort, cleanup, JSONL output, and post-exit
sleep/wake checks passed. The bounded smoke capture is retained at
`results/gate-b-smoke.jsonl`.

Milestone 04 is complete. The matched safe 20 MHz and adaptive 40 MHz smoke,
latency, cadence, and burst captures passed on the available X4 with no rejected
commands. Their raw JSONL, hashes, conditions, caveats, and separated
window/fallback summaries are frozen as `results/baseline-v1.md`.

Milestone 09's first controlled waveform-quality experiment is in progress by
explicit user choice. It keeps the SSD1677 write clock at its specified 20 MHz,
keeps the one-frame profile's directional transitions and X4 analog values, and
changes only the volatile phase-A duration from one to twenty 5 ms frames. The
nominal 100 ms target is not a result; diagnostics must measure its actual BUSY
time and the user must judge contrast, missed-looking updates, and ghosting.

The next pickup is locked into the ordered playbooks under
`docs/knietty-handoff/`: safe state, framed v3, approved diagnostics, controlled
baselines, Rust parity, TLS pairing, then independently measured SSD1677
experiments. BLE keyboard work is explicitly backlogged until the Wi-Fi
terminal, Rust host, encrypted transport, and display scheduler are stable.

## Working features

- The Terminal activity uses CrossPoint's saved-network selector, advertises
  `_knietty._tcp.local`, answers bounded UDP discovery, requires physical
  approval, and carries a raw bidirectional stream over TCP with TCP_NODELAY.
- Protocol v2 negotiates an 80 x 24 PTY and transfers host epoch/time-zone data
  for the header clock. The host falls back to v1 for the 50 x 22 proof of
  concept.
- Protocol v3 preserves v1 discovery and physical approval, then carries typed
  frames with an eight-byte network-order header and a 512-byte payload cap.
  Firmware uses one fixed 512-byte decoder payload and one fixed 1 KiB TX ring;
  these are allocated once with the Terminal activity and never per frame. Host
  `--protocol 2` and `--protocol 1` force compatibility paths.
- `knietty diagnose --suite {smoke,latency,cadence,burst} --output PATH`
  negotiates the separate `frame,diag1` capability without spawning a PTY. Its
  fixed command set covers deterministic cell/row/scroll/window-boundary/large
  and 1/2/5/10/25/100-cell burst patterns, polarity, clean, and stop; raw
  controller controls are not exposed.
- Cadence sends six tagged updates at each of 600/400/200/100/50/25 ms. While
  one refresh executes, firmware merges later named requests into one fixed
  pending screen state and reports its exact first/last sequence and count.
  There is no per-command firmware allocation or dynamically sized queue.
- SSD1677 timing now distinguishes activation-to-BUSY completion, exact BUSY-fall
  presentation timestamp, post-waveform baseline, optional power-off, and final
  READY timestamp. Firmware sends fixed 108-byte network-order refresh records;
  JSON serialization and host monotonic timestamps remain host-only.
- The fixed terminal model supports delayed VT wrapping, scrolling, dirty
  column spans, cursor state, basic CSI/SGR, and incremental UTF-8. Invalid or
  non-BMP input consumes one replacement cell.
- Terminus 8 x 16 is the default flash-resident terminal font. Spleen remains
  an explicitly selectable 1,001-glyph profile covering Latin, Greek, Cyrillic,
  box/block drawing, Braille, and a small Powerline subset. An eight-pixel left
  bezel inset is recovered from eight cell gutters, so all 80 columns still end
  exactly at pixel 800.
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
- Terminal's renderer ownership is temporary: it captures the effective fading
  fix and fast-refresh profile, disables the extra fading-fix display pass while
  active, and restores the exact captured values on exit. Render requests that
  arrive during an E Ink update are coalesced into one guaranteed replay rather
  than being dropped.
- The host creates an isolated PTY session/process group. Local Ctrl+C is
  written to that PTY; Ctrl+\\ exits the bridge even during retry waits. An
  established disconnect exits by default, while `--reconnect` enables daemon
  rediscovery. systemd and launchd templates select the latter.
- tmux remains the preferred child command when installed, preserving the
  session across bridge/device reconnects; `$SHELL` is the fallback.

## Known failures

- The one-frame adaptive waveform has poor contrast and excessive ghosting.
  Adaptive 20 MHz is not fast enough to justify that quality loss. Adaptive
  40 MHz is meaningfully faster and usable as an experiment, but safe 20 MHz
  remains the best overall experience. In the baseline-v1 debrief the user
  reported occasional updates with no obvious visible reaction, some ghosting,
  and contrast better than the previous adaptive attempt but still inadequate.
- The nominal 100 ms / 20 MHz quality waveform has not yet been run on hardware.
  It must not be described as better until both diagnostics and visual testing
  support that conclusion.
- The new font is deliberately bounded and is not a full Nerd Font. Applications
  that require sixel, emoji, combining-cell shaping, or unimplemented xterm CSI
  behavior will still degrade.
- Linux behavior has not been tested. macOS testing alone does not establish
  Linux parity.
- The Wi-Fi protocol is unencrypted and unauthenticated beyond physical
  approval. Use it only on a trusted LAN.
- The Milestone 04 capture did not record ambient temperature, external-power
  state, optical onset, or a capture-specific subjective quality score. Its
  electrical timings are valid, but later optical/quality comparisons must
  record those missing conditions rather than infer them.
- The latest safe-profile diagnostics show the CrossPoint sunlight-fading fix
  was active during the first Terminal capture: it disabled every window update
  and powered the SSD1677 down after every refresh. The subsequent setting-off
  A/B test confirmed the approximately 200 ms penalty disappears and windowing
  resumes. Automatic temporary disable/restore and the approval-prompt render
  fix were subsequently validated on the physical X4.
- Directed broadcast can be filtered by guest Wi-Fi/client isolation; explicit
  `--host` is the fallback.
- The safe controller profile cannot approach 30 or 60 Hz: its measured BUSY
  waveform is about 503 ms even for one cell. Adaptive-40 completed almost all
  small-window 25 ms cadence requests electrically, but its known ghosting and
  weak contrast prevent a legible-frame-rate claim. Raw-panel high-refresh
  projects still have per-pixel waveform and drive controls the SSD1677 command
  interface does not expose.
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
- SSD1677 Display Mode 2 exposes volatile RAM ping-pong through display-option
  register `0x37` bit F6. If it behaves as documented on the X4, the controller
  can swap old/new RAM roles after activation and eliminate knietty's manual
  post-refresh BW/RED baseline rewrite. Do not issue the separate waveform or
  display-option OTP programming commands; they are irreversible and provide no
  benefit over volatile register/LUT testing.
- Driver Output Control `0x01` changes the gate MUX count, but the documented
  SSD1677 range is 300-680 lines and scanning is edge-anchored rather than an
  arbitrary dirty-row window. A separate 800 x 300 speed viewport could test
  whether scanning 300 instead of 480 gates shortens BUSY, at the cost of about
  nine terminal rows. Ordinary RAM windowing reduces transfer bytes but leaves
  all 480 gates configured.
- Blocking display calls now record total, BUSY/waveform, and non-waveform time.
  The SSD1677 additionally records LUT upload, initial plane transfer, and
  post-waveform baseline synchronization. Terminal adds queue and render time,
  freezes the connected-session snapshot while showing diagnostics, and excludes
  non-terminal frames from its averages.
- `GfxRenderer::displayWindow()` deliberately falls back whenever the global
  sunlight-fading fix is enabled, and full-buffer rendering passes that setting
  to the driver as `turnOffScreen=true`. The X4 `0xFC` partial sequence otherwise
  keeps the controller powered, so the driver follows it with its separate
  power-off operation containing a fixed 200 ms delay. The safe hardware capture
  below has 201.3 ms of transfer time not attributed to planes or baseline,
  matching that path to measurement precision.
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
- Protocol v1/v2 becomes an unframed byte stream after approval. Arbitrary
  terminal bytes therefore cannot safely share that stream with telemetry by
  reserving an escape sequence. Diagnostics should introduce a negotiated v3
  framed stream on the existing discovery service and TCP port; v1/v2 must
  remain available for compatibility.

### Host-controlled diagnostics design

- `KNIETTY/3` negotiates `terminal` or `diagnostics` mode and capabilities in
  the greeting. Diagnostics uses the existing named-host approval screen but
  explicitly says that the host is requesting a display test. It does not
  spawn a PTY or mix shell output into the measurements.
- After approval, both directions use a bounded binary frame: type, flags,
  16-bit payload length, and 32-bit sequence number. Frame types cover terminal
  data, device input, control request/response, refresh telemetry, presented
  acknowledgement, and heartbeat. TCP already provides ordering and integrity;
  TLS can later wrap this unchanged stream. Reject unknown types and payloads
  above a small fixed limit rather than allocating from an untrusted length.
- The Python/uv host remains the reference implementation until v3 is tested.
  Its diagnostics command writes JSON Lines: one immutable session/build record
  followed by accepted, PRESENTED, and READY records for every refresh request.
  Rust should port this frozen behavior rather than inventing a second protocol
  during migration.
- Firmware timestamps remain monotonic and relative, so host and X4 clocks do
  not need synchronization. The `PRESENTED` timestamp is captured exactly when
  BUSY falls and `READY` follows baseline synchronization/power handling; this
  distinction directly measures work that delays the *next* activation after
  the new image is already on the panel. The current blocking renderer sends
  both events after READY, so device timestamps—not host receive spacing—measure
  that gap. Each event identifies the first and last included sequence and
  coalesced count. Final telemetry reports
  RX/parse/queue/render time, LUT upload, first-plane transfer,
  activation-to-BUSY assertion, BUSY/waveform, baseline synchronization,
  power-off, total display time, actual dirty/aligned rectangle and bytes,
  changed rows/cells, requested and actual refresh path, and a stable
  fallback-reason code. The host separately records send and event-receive
  monotonic times for end-to-end latency.
- Each session record includes firmware and FreeInk revisions, diagnostics
  schema, board/controller/resolution, safe/adaptive profile, SPI frequency,
  font/orientation/polarity, sunlight-fading state, battery, free/minimum heap,
  Wi-Fi RSSI, and host OS/version. Ambient and panel temperature must be entered
  as external observations unless a trustworthy panel sensor is identified.
- Initial bounded suites are: `smoke`; `latency` for top/middle/bottom cells,
  adjacent cells, cursor-sized, one-row, disjoint-row, scroll, 8 KiB boundary,
  and near-full regions in both directions; `cadence` at
  25/50/100/200/400/600 ms input spacing; and deterministic
  1/2/5/10/25/100-cell `burst` updates. An opt-in
  `ghosting` suite alternates known patterns for bounded counts and finishes
  with a clean. A phone high-speed-video capture of host action plus panel is
  still required to measure visible onset; SSD1677 BUSY completion is only a
  firmware-observable proxy for presentation.
- The host may select only compiled, whitelisted patterns and safe profile
  choices. Cap repetitions and duration, rate-limit commands, abort on Power,
  Back, disconnect, or timeout, and restore orientation, display profile,
  sunlight-fading state, and controller power state on every exit. Never expose
  raw SSD1677 register writes, voltage controls, OTP commands, or an arbitrary
  overclock command over the network.

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
- The `500d757d` safe 20 MHz, adaptive 20 MHz, and adaptive 40 MHz artifacts
  were subsequently flashed successfully. Safe is the preferred-quality build.
  Adaptive 20 MHz is an unattractive middle ground; adaptive 40 MHz is
  noticeably faster and the only adaptive profile currently fast enough to
  justify experimentation, but it retains weak contrast and ghosting.
- The user validated the `4db85157` Gate A safe artifact. The approval prompt
  that previously accepted input without painting is visible, and the
  terminal-owned display-state/sleep-wake checklist passed. No quantitative
  timing capture was taken during this gate.
- The user validated the `fb517134` Gate B safe artifact. Ordinary v3 terminal
  operation, forced v2 compatibility, diagnostics approval/deny/abort, smoke
  JSONL output, cleanup, and post-exit Home sleep/wake passed. The user reported
  no failure in this checklist.
- During the Milestone 04 campaign, mDNS proved that an initially selected
  image was an older adaptive-20 build (`proto=2` and the on-device label
  `Adaptive DU / 20 MHz experimental`), not the intended safe baseline. The
  short artifact alias `knietty-M4-ae82c301-SAFE.bin` was then flashed and the
  complete safe campaign passed. The first adaptive-40 shell loop exposed a
  one-shot host discovery race while the X4 cleaned up the preceding session.
  After the host began re-probing every 250 ms, all four adaptive-40 suites also
  passed.
- No USB CDC `/dev/cu.usbmodem*` node was observed on the connected Mac.
- No partition table, bootloader, secure-boot, or eFuse changes were made.

## Linux host observations

Not tested. The implementation uses POSIX PTY/select/socket APIs and includes a
user systemd template with `--reconnect`, but that is source-level portability
only.

## macOS host observations

- Development host: Darwin/macOS, shell `/bin/zsh`.
- The host suite passes 38/38 tests. Coverage includes discovery and protocol
  parsing, portable PTY sizing/environment, raw local terminal restoration,
  Ctrl+\\ during retry, log rate limiting, and a live subprocess assertion that
  Ctrl+C signals only the PTY child process group. It now also covers diagnostic
  codecs, ordered JSONL telemetry without a PTY, and 32-bit device-clock wrap.
- The proof-of-concept, `0217ada8`, and `60c30d06` bridges were physically
  exercised through discovery, approval, connection, and interactive output.
  `60c30d06` confirmed the default bridge exits on a clean device disconnect.
- `fb517134` was physically exercised on Darwin 25.5.0 through ordinary v3,
  forced v2, and the separately approved diagnostic path. Its smoke capture
  completed with no rejected commands and wrote a structurally complete JSONL
  result.
- Automatic Wi-Fi discovery now retransmits every 250 ms throughout its bounded
  timeout. This fixes the observed back-to-back campaign race where the next
  process sent its sole UDP probe before the X4 returned to listening. The host
  suite passes 38/38 with a deterministic missed-first-probe regression test.
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
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_100ms

python3 scripts/generate_terminal_font_gallery.py
```

Formatting passes with clang-format 21.1.8 installed in the local uv-managed
`.venv`. The host suite passes 38/38 and the native suite passes 160/160. The
Milestone 04 checkpoint `ae82c301` safe build reports RAM 54,268 / 327,680
bytes and flash 5,649,385 / 6,553,600 bytes. Adaptive 40 MHz reports RAM 54,292
bytes and flash 5,650,353 bytes. The expanded suites add no linker RAM to safe
and no dynamic firmware queue; their fixed aggregation metadata lives in the
Terminal activity's one-time heap allocation. These are linker figures, not
runtime heap measurements.

The nominal 100 ms / 20 MHz source checkpoint passes formatting, 38/38 host
tests, 160/160 native tests, its dedicated firmware build, and the unchanged
safe firmware build. Before final version stamping, the experimental build
reported RAM 54,292 / 327,680 bytes and flash 5,650,719 / 6,553,600 bytes; safe
reported RAM 54,268 bytes and flash 5,649,753 bytes. The extra LUT is 112 bytes
of flash-resident constant data and adds no runtime allocation. These figures
are software-only until the image is flashed and measured on the X4.

The retained nominal 100 ms / 20 MHz experiment is:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-e4238425-20MHz-EXPERIMENTAL.bin
```

It is 5,664,576 bytes and has SHA-256
`274d0ac1f4107f866baa76cc994326b0e2175553b095ce1aa2fcb64a95d27bf1`.
It embeds CrossPoint version
`1.5.0-dev-feature/knietty-terminal-e4238425` and FreeInk revision `2218b6c`.
This artifact is experimental and software-validated only. The retained
`knietty-M4-ae82c301-SAFE.bin` remains the hardware-tested rollback control.

The Milestone 04 baseline artifacts are:

```text
/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-SAFE.bin
```

The safe 20 MHz image is 5,663,232 bytes and has SHA-256
`49aacc5b1c32da48e0a4e09bf9c76be40d3ce198559200b68c29b93ad3d451b3`.

```text
/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-ADAPT40-EXPERIMENTAL.bin
```

The adaptive 40 MHz image is 5,664,208 bytes and has SHA-256
`aff854e1f46218e966ffc77a0f5df041d135fb830316ca3ec15ace5afa6c070f`.
It remains explicitly experimental because its SSD1677 SPI rate exceeds the
board's normal 20 MHz setting. Both images embed CrossPoint version
`1.5.0-dev-feature/knietty-terminal-ae82c301` and FreeInk revision `0ff05c6`.
They are software-validated only until the two matched physical captures pass.

The Milestone 01 Gate A artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-4db85157-80x24-terminus-safe-20mhz-gate-a.bin
```

It was rebuilt from source checkpoint `4db85157`, is 5,655,456 bytes, embeds
version `1.5.0-dev-feature/knietty-terminal-4db85157`, and has SHA-256
`bd45f2f807ea71e81ce64d30d85a73cc1ad77b2bfade65a2e30d466fe23efc24`.
The user subsequently validated its physical Gate A checklist on the available
X4; no new performance measurement was reported during that validation.

The Milestone 03 Gate B artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-fb517134-80x24-terminus-safe-20mhz-gate-b.bin
```

It was rebuilt from source checkpoint `fb517134`, is 5,662,768 bytes, embeds
version `1.5.0-dev-feature/knietty-terminal-fb517134` and FreeInk revision
`0ff05c6`, and has SHA-256
`e0e22ae5919a77c8256f35ef414ee7f4dd641add956aad2ca3c5fbc72b133d0c`.
The user physically validated its Gate B checklist on the available X4.

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

The current software-tested Terminus artifacts all embed
`1.5.0-dev-feature/knietty-terminal-500d757d`:

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-safe-20mhz.bin
```

This stock-driver safety baseline is 5,655,280 bytes and has SHA-256
`ab8cb750ce65fa9c64f3406db24ac79d0987193b7cc0dbde6a7c0b84c76c6b32`.

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-adaptive-20mhz.bin
```

This adaptive 20 MHz image is 5,656,160 bytes and has SHA-256
`6d24add5853067b8f37864627128f45dd3be003708645e18428c2103bfdf60fa`.

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-adaptive-40mhz-EXPERIMENTAL.bin
```

This adaptive 40 MHz image is 5,656,160 bytes and has SHA-256
`087aa68e0b7cf84313316261a04acc9dc1a1b7bf8d714c9ec198b685cf594c02`.
It drives the SSD1677 SPI link beyond the board's normal 20 MHz setting and
must remain clearly labeled experimental. All three `500d757d` profiles have
now been flashed and qualitatively compared on the available X4. One safe
profile capture is recorded below; equivalent adaptive captures are still
missing.

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

The physically validated Gate B safe smoke capture is retained verbatim in
`results/gate-b-smoke.jsonl` (SHA-256
`b0712b58e78afc282089b4ae0413e13b36c8df5982ce4745aa6e8b8dcaa6d2d0c`).
It identifies build `fb517134`, FreeInk `0ff05c6`, an 800 x 480 display, safe
profile, 20 MHz SPI, 80 x 24 Terminus geometry, battery 70%, RSSI -57 dBm,
72,572 bytes free heap, and a 60,112-byte minimum observed heap.

The file contains one session record, 14 accepted command responses, and 13
correctly ordered PRESENTED/READY pairs with no rejection. Excluding the reset
and final clean, the four true window activations averaged 2.079 ms queued,
1.520 ms rendered, 2.716 ms transferred, 504.074 ms in the waveform, 0.726 ms
in baseline synchronization, and 506.790 ms total. The smallest one-cell case
used 0.483 ms render, 1.968 ms transfer, 504.013 ms waveform, 0.371 ms baseline,
and 505.981 ms total.

Seven larger patterns exceeded the 8 KiB window cap and used full-frame fast
fallback. They averaged 2.796 ms queued, 70.821 ms rendered, 109.539 ms
transferred, 503.650 ms in the waveform, 73.051 ms in baseline synchronization,
and 613.189 ms total. The final deliberate HALF clean took 1,839.792 ms,
including a 1,694.897 ms waveform, and is not an interactive measurement.

This controlled Gate B capture isolates the main safe-mode limit: even a one-cell
window spends about 504 ms in the SSD1677 activation/BUSY interval while all
firmware work before it takes only a few milliseconds. Windowing substantially
reduces render, transfer, and baseline work, but it does not shorten the stock
safe waveform. The initial reset's 656.022 ms queue is setup behavior and is
excluded from the interactive averages. The later baseline-v1 cadence and burst
suites quantify overlapping arrivals and latest-frame scheduling below.

The user subsequently recorded this 135-update safe-profile diagnostic on the
physical X4:

```text
Stock X4 partial / 20 MHz safe
Last 1642.8 ms; waveform 504.7 ms
Queue 775.0 ms; render 59.5 ms
Transfer 303.5 ms: plane 33.3 ms, LUT 0.0 ms, baseline 68.9 ms
Average 1537.0 ms; minimum 815.4 ms; maximum 1815.3 ms
Updates 135; window 0; fallback 135; settle 0; clean 0
Last region 800 x 472 / 47,200 bytes
```

This is a near-full-screen workload: the 47,200-byte region exceeds the current
8 KiB transient-window cap. More importantly, all 135 updates fell back. The
reported transfer subphases account for 102.2 ms, leaving 201.3 ms of the
303.5 ms transfer total unexplained by RAM traffic. That matches the driver's
fixed 200 ms post-refresh power-off delay and, together with the forced window
fallback, is strong evidence that CrossPoint's sunlight-fading fix was enabled.

The end-to-end `Last` value is also not one 1.64-second physical refresh. It is
775.0 ms waiting behind the preceding update followed by about 867.7 ms of this
update's render, transfer, and waveform. This confirms both a real 504.7 ms safe
waveform ceiling and a separate firmware scheduling/power penalty.

After disabling CrossPoint's sunlight-fading fix without reflashing, the user
repeated the safe-profile diagnostic:

```text
Stock X4 partial / 20 MHz safe
Last 1253.1 ms; waveform 503.7 ms
Queue 503.0 ms; render 85.9 ms
Transfer 160.3 ms: plane 34.9 ms, LUT 0.0 ms, baseline 125.2 ms
Average 1151.3 ms; minimum 628.6 ms; maximum 1402.2 ms
Updates 68; window 14; fallback 54; settle 0; clean 0
Last region 800 x 472 / 47,200 bytes
```

This confirms the source diagnosis. Waveform time was unchanged within 1 ms,
while the previously unexplained 201.3 ms disappeared: the new 160.3 ms
transfer total is almost exactly its 34.9 ms plane plus 125.2 ms baseline.
Windowing also resumed (`14/68` rather than `0/135`). The last frame was still a
47,200-byte near-full-screen update and therefore correctly exceeded the 8 KiB
window cap. Last latency improved by 389.7 ms and average latency by 385.7 ms;
the improvement combines removal of the power-down penalty with a shorter queue.

The 125.2 ms two-plane baseline is unexpectedly high relative to the 34.9 ms
single-plane transfer and should be measured again under a controlled workload.
Regardless, it strengthens the case for Mode 2 RAM ping-pong, which is intended
to eliminate that manual post-waveform baseline synchronization.

On the physically tested Terminus turbo image, after 50 displayed updates, the
user recorded: last 526.4 ms, waveform/BUSY 226.5 ms, transfer 164.7 ms, render
134.9 ms, average 576 ms, minimum 459 ms, and maximum 1975 ms. Those values came
from the first metrics implementation, which mixed waiting, approval,
diagnostics, first-frame, and disconnect/clean paints with terminal updates. In
particular, the 1975 ms maximum is consistent with a HALF clean and is not a
valid interactive maximum. Baseline v1 supersedes these mixed-path values for
controlled safe/adaptive comparisons.

The observed 226.5 ms BUSY interval establishes a physical upper bound of about
4.4 completed global activations per second for that older turbo waveform on
this unit, before rendering and SPI work. It is not the waveform used by the
new adaptive-40 diagnostic profile.

Baseline v1 is retained at `results/baseline-v1.md` with all eight raw JSONL
files and SHA-256 hashes. Across latency, cadence, and burst workloads, safe
window activations had a 503.223 ms median BUSY waveform and 505.213 ms median
display-call total. Safe began coalescing between the tested 600 and 400 ms
cadences; at 25 ms, 12 requests became four activations. Full-frame fallback
raised safe median display total to 609.649 ms, primarily through 35.780 ms of
plane transfer and 70.034 ms of baseline synchronization.

Adaptive-40 window activations had a 5.302 ms median BUSY waveform and 7.341 ms
median display-call total. It preserved all 12 requested activations down
through 50 ms cadence and merged one pair at 25 ms. Its fallback median was
65.427 ms, including 20.212 ms of plane transfer and 38.889 ms of baseline
synchronization. This two-image comparison changes both waveform and SPI rate,
so it cannot attribute their entire difference to 40 MHz. The electrical speed
also does not overturn the user's earlier weak-contrast/ghosting observation.
The next waveform-quality experiment should return SPI to the specified 20 MHz
and change only the volatile directional LUT duration toward approximately
100 ms. The measured small-window plane-time benefit from the current 40 MHz
comparison is only about 0.25 ms at the median, too small to justify confounding
a 100 ms waveform trial with an out-of-spec bus clock.

## Last known-good commit

- `e4238425` is the nominal 100 ms / 20 MHz software checkpoint and points to
  FreeInk `2218b6c`. It passes formatting, 38/38 host tests, 160/160 native
  tests, the dedicated experimental firmware build, and the unchanged safe
  build. It is not hardware-known-good until the X4 flash and bounded smoke
  gate pass.
- `ae82c301` is the current firmware hardware-known-good checkpoint and points
  to FreeInk `0ff05c6`. It passes 37/37 checkpoint host tests, 160/160 native
  tests, formatting, both firmware builds, and the complete safe/adaptive
  baseline-v1 hardware campaign.
- `2880ba38` is the current host-known-good checkpoint. It passes 38/38 host
  tests and physically fixed back-to-back diagnostic rediscovery without a
  firmware change.
- `fb517134` is the previous hardware-known-good checkpoint and points to FreeInk
  `0ff05c6`. It passes 36/36 host tests, 160/160 native tests, the safe firmware
  build, ordinary v3 and forced-v2 terminal checks, and the full Gate B
  diagnostics checklist.
- `4db85157` is the earlier Milestone 01 hardware-known-good checkpoint. It
  passes 24/24 host tests, 152/152 native tests, formatting, the safe firmware
  build, and the user-confirmed Gate A checklist.
- `500d757d` is the current software- and hardware-tested knietty checkpoint and
  points to FreeInk commit `60b040f`. Host tests pass 24/24, native tests pass
  149/149, and all three firmware profiles build and boot. Safe 20 MHz is the
  preferred baseline; adaptive 40 MHz remains experimental.
- `60c30d06` is the latest physically booted terminal checkpoint. Its sleep/wake,
  icon, btop/glyph, exit, and host-disconnect behavior are known good; its
  remaining UI/latency observations are recorded above.
- `b80046b3` documents the physically tested Terminus turbo artifact and points
  to FreeInk commit `72ff720`.
- Official CrossPoint 1.5.0 is the physically tested recovery firmware.
- `33f07db7` is the built unmodified upstream baseline.

## Next concrete step

Milestones 01–04 are complete. Continue with the explicitly selected waveform
experiment, then return to the ordered handoff:

1. Build and SD-flash the nominal 100 ms / 20 MHz image. Confirm its timing page
   label, run bounded smoke diagnostics, and compare normal typing contrast and
   residue against the retained safe and adaptive-40 controls.
2. If smoke and visual inspection pass, run latency, cadence, and burst into a
   separately named result set. Record actual BUSY timing and qualitative notes;
   retain or reject the profile before changing another LUT field.
3. Migrate the host bridge from Python to Rust while keeping the Python/uv
   implementation as the behavioral reference until discovery, PTY handling,
   terminal restoration, reconnect, diagnostics, tmux, Linux, and macOS parity
   tests pass.
4. Add mutually authenticated TLS to the Rust transport, including persistent
   device/host identity and first-pair human verification tied to the X4's
   physical approval flow. Keep plaintext only as an explicitly selected
   trusted-LAN development mode.
5. Add a volatile SSD1677 RAM-ping-pong experiment. Seed both complete RAM banks
   once, enable Mode 2 ping-pong without programming OTP, verify bank polarity,
   and measure whether baseline transfers disappear without stale pixels.
6. Split window refresh into asynchronous start/finish and implement
   latest-frame-wins coalescing. Receive and parse during BUSY, prepare the next
   bounded window, and tail-chain it immediately after completion.
7. If the 100 ms point is promising, bracket it with shorter and longer 20 MHz
   drive durations. Then test direction-asymmetric pulses. Replace the inverse
   block cursor with an underline and make quiet-time settling affect only
   pixels/cells that changed.
8. Independently test an SSD1677 300-gate, 800 x 300 speed viewport. Do not
    combine it with waveform changes until its BUSY timing, mapping, recovery,
    and image stability are known.
9. Complete Linux/macOS/device release validation and promote the project
    description into README/package metadata.

Backlog: BLE keyboard input and host relay. Start it only after display latency,
the Rust host, and TLS are stable.

The detailed source anchors, implementation order, verification commands,
hardware gates, and completion criteria are in
`docs/knietty-handoff/README.md` and its numbered milestone files. Those files
are the execution authority; this list is the summary.
