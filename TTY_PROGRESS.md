# Project: knietty

## Current milestone

Wi-Fi-first terminal prototype, built and flashed to the available China-locked
X4. The firmware boots and knietty appears in the CrossPoint menu. USB CDC did
not enumerate, so the active milestone is now:

```text
CrossPoint saved Wi-Fi -> LAN discovery -> physical approval -> TCP -> terminal
```

BLE keyboards and custom E Ink waveform/window experiments are deferred. Beyond
boot and menu visibility, terminal and host behavior have not been validated on
the physical X4 yet.

## Working features

- The conditional `knietty` Home entry launches CrossPoint's existing saved
  Wi-Fi selector, including its normal auto-connect behavior.
- While Terminal is active the X4 advertises `_knietty._tcp.local`, answers
  `KNIETTY/1 DISCOVER` UDP probes on port 29380, and listens for one TCP client
  on the same numbered TCP port.
- The TCP path disables Wi-Fi power saving and enables TCP_NODELAY. Discovery,
  handshake, host name, and address storage use fixed-size firmware buffers.
- A host sends `KNIETTY/1 HELLO <name>`. The X4 shows the sanitized host name
  and source IP; Confirm sends `ACCEPT 50 22`, Back sends `DENY`, and another
  simultaneous host receives `BUSY`.
- After approval the connection becomes a raw, bidirectional terminal byte
  stream. Disconnect returns the X4 to discovery/listen mode.
- Terminal exit stops TCP, UDP, and mDNS, restores the display orientation, and
  follows CrossPoint's existing Wi-Fi activity cleanup/restart pattern.
- The 50 x 22 terminal model, ANSI subset, static 4 KiB RX ring, fixed 16 x 20
  cell renderer, mapped button input, tmux PTY, and USB legacy host mode remain.
- Wi-Fi output is paced at 16,384 bytes/s by default; PTY state survives network
  reconnects through the same bridge process and, by default, tmux.
- Host auto-discovery is dependency-free and works through one IPv4 LAN
  broadcast. `--host IP_ADDRESS` bypasses discovery on networks that filter
  broadcasts.
- Interactive host invocations automatically place local stdin in cbreak mode
  and forward Mac keyboard bytes into the PTY; terminal settings are restored
  on exit. Headless daemon invocations leave local input disabled.
- Linux user-systemd and macOS LaunchAgent templates now start Wi-Fi mode.
- The terminal build enables the X4 driver's existing opt-in fast-DU shortcut,
  batches bursts after 8 ms (20 ms maximum), performs a HALF clean after 20
  fast updates, and performs an idle clean after four seconds.

## Known failures

- The available X4 is a China-locked unit that previously ran CrossPoint 1.4.1
  installed through the web unlock tool. The user retained the 1.4.1 binary and
  successfully flashed the knietty build, but restoring 1.4.1 has not been
  exercised during this work.
- No USB CDC node appeared on the development Mac. USB is a legacy code path,
  not the planned transport.
- Wi-Fi discovery and the approval/TCP handshake now work on hardware. Terminal
  output rendering, keyboard echo, X4 button input, reconnect, refresh cadence,
  runtime free heap, and battery behavior still need validation.
- Linux behavior has not been tested. macOS host tests do not establish Linux
  parity.
- The protocol is not encrypted or authenticated beyond physical approval. It
  is a trusted-LAN prototype.
- Directed broadcast can be filtered by guest Wi-Fi/client-isolation rules;
  explicit `--host` remains the fallback.
- 30 Hz and 60 Hz are not current claims. Even the source-documented fast-DU
  shortcut is about 77 ms before application overhead, so its theoretical
  ceiling is below 13 Hz. Actual rate and visual quality must be measured.
- Repository formatting could not run because `clang-format` is absent on the
  development Mac. `git diff --check` passes.

## Architecture findings

- Upstream baseline is `develop` at `33f07db7`; source version is CrossPoint
  1.5.0. The device currently reports CrossPoint 1.4.1.
- Toolchain: pioarduino PlatformIO Core 6.1.19, platform-espressif32 55.3.37,
  Arduino-ESP32 3.3.7, ESP-IDF 5.5.2, RISC-V GCC 14.2.0.
- X4 geometry is 800 x 480, SSD1677, with one 48,000-byte 1-bpp framebuffer.
- `HalDisplay::FAST_REFRESH` maps to the SSD1677 fast waveform. Source comments
  document roughly 500 ms for the stock path, roughly 77 ms for the opt-in X4
  fast-DU shortcut, HALF at 1720 ms, and FULL around 1800 ms. These are source
  values, not knietty measurements.
- `FreeInkDisplay::displayWindow()` and a byte-aligned SSD1677 implementation
  exist, but the method is not exposed through `HalDisplay`/`GfxRenderer` and
  the driver allocates temporary vectors. This checkpoint keeps whole-buffer
  FAST updates and does not claim rectangular refresh.
- Bundled UI fonts are proportional. The existing knietty bitmap ASCII font is
  fixed-width and consumes no runtime glyph heap.
- Physical X3/X4 inputs map to logical Back, Confirm, Left, Right, Up, Down, and
  Power. Terminal input uses `MappedInputManager`, not GPIO/button IDs.
- CrossPoint's Wi-Fi selector owns stored credentials and leaves a successful
  connection alive for its parent activity. Network activities stop mDNS,
  disconnect Wi-Fi, then use `silentRestart()` on exit; Terminal follows that
  established lifecycle.
- Arduino `NetworkServer`, `NetworkClient`, and `NetworkUDP` provide nonblocking
  accept/available/read APIs. TCP_NODELAY is available in Arduino-ESP32 3.3.7.
- Native USB is Arduino-ESP32 `HWCDC`; its RX APIs are nonblocking, but no active
  device node was observed. The knietty environment still omits serial logging.

## Hardware observations

- Device: China-locked XTEINK X4.
- Previous firmware: CrossPoint 1.4.1, installed using the web flash unlock
  tool. Its binary is retained and accepted by the device's installer.
- Current firmware: the local knietty build flashed successfully; it boots and
  the knietty Home-menu item is visible (reported by the user on 2026-08-15).
- LAN discovery returned `knietty-9e54a0` at `192.168.0.251:29380`; the X4
  approved the request and established the TCP session at 50 x 22.
- USB CDC: no `/dev/cu.usbmodem*` or matching serial metadata appeared on the
  connected development Mac.
- Wi-Fi, screen timing, controls, free heap, flash/recovery, and battery: not
  tested.

## Linux host observations

Not tested. The code uses POSIX PTY/select/socket APIs and a user systemd
template, but this is source-level portability only.

## macOS host observations

- Development host: Darwin/macOS.
- `uv` host suite: 18/18 tests pass, covering USB legacy selection, UDP response
  parsing, protocol approval responses, ambiguity rejection, PTY geometry,
  child environment, local terminal restoration, and CLI defaults.
- LAN discovery consistently finds the physical X4 at `192.168.0.251:29380`.
- Physical approval completes and the host reports a connected 50 x 22 session.
- The first interactive attempt exposed that local Mac stdin was not forwarded;
  the host fix is unit-tested but has not yet been retried against the X4.
- LaunchAgent template passes `plutil -lint`.
- No physical network bridge or terminal session has been tested.

## Build commands

An isolated official `uv` binary was used because `uv` is not on the host PATH:

```sh
curl -LsSf https://astral.sh/uv/install.sh |
  env UV_UNMANAGED_INSTALL=/private/tmp/knietty-uv sh

/private/tmp/knietty-uv/uv run --project host --no-sync \
  python -m unittest discover -s host/tests -v

PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  /private/tmp/knietty-platformio/penv/bin/pio run -e knietty
```

The current `knietty` build succeeds. PlatformIO reports RAM 54,228 / 327,680
bytes (16.5%) and flash 5,618,319 / 6,553,600 bytes (85.7%). These are linker
figures, not runtime heap measurements.

`uv sync --project host` could not be refreshed in this session because PyPI
timed out. The dependency set is unchanged (`pyserial` only), the existing
uv-managed environment was used with `--no-sync`, and `host/uv.lock` did not
need to change.

## Flash/update commands

No flash command is approved for the current locked unit. In particular, do not
run PlatformIO upload or esptool against it. The upstream command for ordinary,
unlocked hardware is `pio run -e knietty -t upload`, but it is not a verified
path for this device and has not been run.

## Recovery procedure

The custom-image installation route is now proven, and the known-good 1.4.1
binary is retained. Recovery is only partially verified:

1. Retain the exact known-good CrossPoint 1.4.1 firmware used on the device.
2. Keep the partition table unchanged and preserve the accepted 1.4.1 image.
3. If knietty fails before network validation can continue, restore 1.4.1 and
   record whether the restoration completes successfully.

## Performance measurements

None. The 8/20 ms batching values, 16 KiB/s host pacing, and driver-comment
waveform durations are configuration/source facts, not observed performance.

## Last known-good commit

`fd30bdd1` is the latest committed software-only documentation checkpoint.
Current Wi-Fi work is uncommitted, passes the build and host tests above, and is
known to boot far enough to display its menu entry. Terminal operation is not
yet known-good. `33f07db7` remains the built unmodified upstream baseline.

## Next concrete step

Open **knietty** on the X4 and let it join the saved network, then run:

```sh
uv run --project host knietty --list-devices
uv run --project host knietty --host auto --verbose
```

Confirm the displayed host request on the X4, type a short command on the Mac,
and verify that shell echo/output appears on the X4. Then test X4 Confirm and
arrow input. Only after that smoke test should latency, refresh duration,
ghosting, reconnect, runtime heap, and battery use be measured. Linux validation
remains a separate follow-up.
