# Project: knietty

## Current milestone

The software implementation for milestones 2 through 6 is complete as an
unflashed prototype: firmware builds, host tests pass on macOS, and portable
Linux/macOS integration templates are present. Milestone 1 remains deliberately
open at the hardware recovery gate: inventory, backup, flashing, recovery
verification, physical validation, and measurements require a connected XTEINK
X4. No hardware claim in this document is inferred from a build or simulation.

## Working features

- Recursive upstream checkout on `feature/knietty-terminal`.
- Unmodified `develop` firmware builds successfully on macOS.
- Source-level X4 display, refresh, input, USB, build, and activity findings are
  recorded below.
- A conditional `knietty` firmware environment disables serial logging and adds
  a Home menu entry without changing the normal `default` environment.
- `TerminalActivity` coordinates CDC input, mapped buttons, a bounded terminal
  screen/parser, dirty-row framebuffer drawing, burst batching, and periodic
  full refreshes.
- The terminal screen uses 2,200 bytes for 50 x 22 two-byte cells. It implements
  printable ASCII, CR/LF/backspace/tab, wrapping, scrolling, cursor visibility,
  CSI cursor motion, clear screen/line, and monochrome SGR attributes.
- A public-domain 8 x 8 IBM VGA-derived ASCII bitmap is rendered as fixed 16 x
  20 cells; the default landscape grid is 50 columns by 22 terminal rows plus a
  status row.
- Mapped buttons emit arrows, Enter, Escape, and long-press Ctrl+C; long Back
  exits through the normal Activity stack.
- CDC bytes are received into a statically bounded lock-free 4 KiB queue so the
  main loop can continue draining the 256-byte hardware queue during a display
  refresh. Overflow is visible in the terminal instead of corrupting memory.
- The Python host bridge provides scored auto-discovery, explicit device paths,
  a portable PTY with size/environment setup, output pacing, short-write-safe
  forwarding, reconnect, tmux-by-default, and shell fallback.
- Example Linux udev/user-systemd and macOS user LaunchAgent integration is
  included. Device identifiers remain placeholders until the real X4 is
  enumerated.

## Known failures

- No X4 is currently visible under `/dev/cu.usb*`, so enumeration, backup,
  upload, recovery, and on-device behavior have not been tested.
- Linux behavior has not been tested.
- macOS serial behavior has not been tested with hardware.
- The Linux host bridge and integration have not been executed on Linux.
- USB identity filters and the Linux udev rule cannot be finalized until VID,
  PID, product, and serial metadata are observed.
- The host defaults to 2,048 bytes/second pacing as a conservative starting
  value; it is not a measured sustainable device/render rate.

## Architecture findings

- Upstream checkout: `develop` at `33f07db7` (`fix: guard file list against
  render-task race in FileBrowserActivity (#3034)`). The source reports
  CrossPoint `1.5.0`.
- Toolchain: pioarduino PlatformIO Core `6.1.19`, platform-espressif32
  `55.3.37`, Arduino-ESP32 `3.3.7`, and ESP-IDF `5.5.2`.
- X4 geometry is exactly 800 x 480 pixels (`BoardConfig.h`, XTEINK_X4 profile),
  with an SSD1677 controller and a 48,000-byte 1-bpp framebuffer.
- `HalDisplay::FAST_REFRESH` maps through `EInkDisplay::FAST_REFRESH` to the
  SSD1677 fast waveform. The X4 driver documents about 500 ms for the stock fast
  waveform and about 1800 ms for full refresh; these are source comments, not
  measurements made by this project. `HalDisplay` separately documents HALF as
  1720 ms.
- An experimental `FreeInkDisplay::displayWindow()` exists and the SSD1677
  implementation accepts byte-aligned rectangular windows, but it allocates
  `std::vector` window buffers and is not exposed by `HalDisplay` or
  `GfxRenderer`. `FAST_REFRESH` must not be described as rectangular refresh.
- None of the bundled Noto Sans, Noto Serif, or Ubuntu UI fonts is fixed-width.
  knietty therefore needs a compact static ASCII bitmap font.
- X3/X4 use the same two ADC button ladders. Physical inputs expose Back,
  Confirm, Left, Right, Up, Down, and Power. Terminal code must consume the
  logical `MappedInputManager` buttons because the four front controls can be
  remapped in settings.
- `ARDUINO_USB_MODE=1` plus `ARDUINO_USB_CDC_ON_BOOT=1` selects Arduino-ESP32's
  `HWCDC`, backed by the ESP32-C3 USB Serial/JTAG peripheral. `HWCDC::available()`
  and `read()` inspect a FreeRTOS RX queue without waiting, so polling is
  non-blocking. RX defaults to 256 bytes.
- `HWCDC::operator bool()` delegates to the core's CDC-connected state. It is
  cleared when SOF/plug detection fails or on bus reset, and set by traffic/TX
  events; it is useful but is not equivalent to a host-controlled DTR signal.
  CrossPoint's cached HAL USB state combines electrical detection with USB SOF
  activity and is the safer plug-state indicator.
- `Logging.h` deliberately exposes the underlying CDC object as `logSerial`.
  The normal `Serial` name is replaced by a logging wrapper without RX methods.
- Serial logs can be excluded safely with a dedicated build environment that
  omits `ENABLE_SERIAL_LOG`; knietty transport must use `logSerial` so firmware
  logs never enter the terminal byte stream.
- The least invasive Home integration is a conditionally compiled menu item and
  `ActivityManager::goToTerminal()`. Replacing Home with `TerminalActivity`
  avoids retaining Home's cover allocation; `finish()` naturally returns to a
  fresh Home activity.
- Activity `loop()` and render task run independently and share state only under
  `RenderLock`. Terminal mode must restore renderer orientation in `onExit()`,
  use the existing display HAL, and override `preventAutoSleep()` without
  changing global power/deep-sleep behavior.
- The repository documents `pio run --target upload` and direct ESP32-C3 writes
  at offset `0x10000`. The dedicated knietty command will be
  `pio run -e knietty -t upload`, but it is not verified until hardware is
  connected.
- Boot recovery firmware selection is entered by holding Up together with Power
  for roughly the first 500 ms after a power-button wake, then choosing a
  firmware image from the SD card. This source path is not yet hardware-verified.
- USB VID/PID/product/serial strings cannot be established from the active X4
  without enumeration. Hardware Serial/JTAG descriptors are controlled below
  the Arduino native-USB descriptor layer, so they must be recorded from the
  real device rather than inferred from `USB.cpp` defaults.

## Hardware observations

No physical XTEINK X4 observations yet. Installed firmware version, USB lock
state, battery state, and exact physical control labels are unknown.

## Linux host observations

Not tested. Expected candidates such as `/dev/ttyACM*` remain hypotheses until
observed. Record `lsusb`, kernel attach logs, device nodes, and `udevadm info`
when Linux hardware testing begins.

## macOS host observations

Development host: macOS (Darwin). No `/dev/cu.usb*` device was present during
inspection. Product, VID, PID, serial number, and BSD device path are unobserved.
Eleven host unit/integration tests pass on macOS, including discovery scoring,
ambiguity rejection, callout-device preference, PTY geometry, child environment,
and CLI parsing. The LaunchAgent template passes `plutil -lint`. These checks do
not validate macOS USB communication with an X4.

## Build commands

Python tooling uses `uv` as requested:

```sh
curl -LsSf https://astral.sh/uv/install.sh -o /private/tmp/knietty-uv-installer.sh
env UV_UNMANAGED_INSTALL="$PWD/.venv/bin" sh /private/tmp/knietty-uv-installer.sh
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  .venv/bin/uv pip install --python .venv/bin/python --upgrade \
  https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e default
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  .venv/bin/uv sync --project host
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  .venv/bin/uv run --project host python -m unittest discover -s host/tests -v
env CPLUS_INCLUDE_PATH=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e default -t unit-tests
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e knietty
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e default
```

The final host suite passes 11/11 tests and the aggregate native C++ suite passes
142/142 tests. The final `knietty` firmware build reports RAM 54,204 / 327,680
bytes (16.5%) and flash 5,609,619 / 6,553,600 bytes (85.6%). The final `default`
regression build reports RAM 54,220 / 327,680 bytes (16.5%) and flash 5,689,663 /
6,553,600 bytes (86.8%). These are linker utilization figures, not runtime
free-heap measurements. The macOS Command Line Tools C++ include path override
is needed for PlatformIO's aggregate native test target on this development
host.

## Flash/update commands

Documented upstream commands, not yet run on this hardware:

```sh
pio run -e knietty -t upload
esptool.py --chip esp32c3 --port DEVICE --baud 921600 \
  write_flash 0x10000 .pio/build/knietty/firmware.bin
```

The upstream web installer also accepts a custom `firmware.bin`. Do not run any
of these until the recovery checklist below is completed with the connected X4.

## Recovery procedure

Before the first experimental flash:

1. Record the installed CrossPoint version and whether USB flashing is locked.
2. Download and retain the matching official `firmware.bin`.
3. Read and retain a 16 MiB flash backup when the device permits it.
4. Verify the normal web/USB flashing path with a known-good CrossPoint image.
5. Copy a known-good image to SD and verify Up + Power reaches the recovery
   firmware picker.
6. Confirm the OTA/update path while still running known-good firmware.
7. Keep the existing partition table unchanged.

The repository also documents external SPI-flash programming as a last-resort
brick recovery procedure; it requires opening the device and is not considered
a substitute for completing the checks above.

## Performance measurements

None. Source comments and linker utilization are recorded separately and must
not be presented as measurements.

## Last known-good commit

`0520925b` is the local software checkpoint that passed the builds and tests
listed above. It has not been flashed or hardware-tested. `33f07db7` remains the
successfully built, unmodified upstream baseline; there is no hardware-known-good
knietty commit yet.

## Next concrete step

Connect the physical X4 and complete the recovery checklist above. Then record
USB metadata and current firmware, validate the stock upload/recovery paths,
flash the `knietty` build, and execute the milestone 2 one-way CDC smoke test.
Linux testing must follow independently before portability is claimed.
