# knietty terminal mode

knietty is an opt-in CrossPoint build plus a user-space host bridge. It retains
the reader firmware and activity lifecycle, uses the ESP32-C3 USB Serial/JTAG CDC
transport already present in CrossPoint, and renders a bounded character-cell
screen into the existing E Ink framebuffer.

Nothing in this document is evidence of physical X4 validation. Check
`TTY_PROGRESS.md` for the exact tested state before flashing.

## Firmware architecture

Build environment `knietty` adds `KNIETTY_ENABLED` and deliberately omits
`ENABLE_SERIAL_LOG`. This is required: terminal bytes and firmware logs cannot
share the unframed CDC stream.

The Home menu opens `TerminalActivity`, which switches the renderer to native
800 x 480 landscape. It owns:

- a fixed 50 x 22 screen of two-byte ASCII cells (2,200 bytes);
- a small VT100-style parser with bounded parameters;
- a static 4 KiB single-producer/single-consumer RX ring;
- a public-domain 8 x 8 bitmap font rendered at 2x in 16 x 20 cells;
- logical CrossPoint button input and CDC transport wrappers.

RX is drained continuously from Arduino `HWCDC`, including while the render task
waits for the panel. Parser mutations occur on the render task, avoiding a race
with framebuffer drawing. Output bursts wait 100 ms after the latest byte or
200 ms from the first byte, whichever happens first. Dirty terminal rows are
redrawn in RAM and sent using one whole-frame `FAST_REFRESH`. A full refresh is
used on entry, after 40 fast refreshes, or after a 3-second clean-idle interval.
These constants are initial values, not hardware-tuned measurements.

Supported input is printable ASCII, LF, CR, backspace, tab, BEL (ignored), line
wrap, scroll, cursor movement, screen/line erase, cursor visibility, and SGR
normal/bold/inverse/underline. ANSI colors are accepted and ignored.

Controls are:

| Logical button | PTY input |
| --- | --- |
| Up / Down / Right / Left | `ESC [ A/B/C/D` |
| Confirm | carriage return |
| Back | Escape |
| Confirm held at least 1 second | Ctrl+C |
| Back held at least 1 second | leave terminal mode |

The logical mapping honors CrossPoint's configured front-button mapping; no
physical GPIO identifiers are embedded in terminal code.

## Build

This checkout pins pioarduino PlatformIO Core 6.1.19 in a local uv-managed
environment. The reproducible development commands are:

```sh
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  .venv/bin/uv pip install --python .venv/bin/python --upgrade \
  https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip

env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e knietty
```

The output is `.pio/build/knietty/firmware.bin`.

## Mandatory pre-flash recovery gate

Do not flash the feature build until all of these are complete and recorded in
`TTY_PROGRESS.md`:

1. Record the X4's installed firmware version and USB lock state.
2. Store the matching official CrossPoint release binary.
3. If normal USB reads are permitted, back up the full 16 MiB flash and verify
   the backup file size.
4. Prove the documented web/USB flash path with a known-good CrossPoint image.
5. Put a known-good image on SD and verify the Up + Power recovery picker.
6. Verify the normal OTA/update route.
7. Record VID, PID, product, serial, and device nodes on both target hosts.

No partition changes are part of knietty.

After that gate, the repository documents the following upload command; replace
`UPLOAD_PORT` with the observed port:

```sh
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e knietty -t upload --upload-port UPLOAD_PORT
```

The upstream web flasher's Custom `.bin` path is an alternative. Direct flashing
uses ESP32-C3 offset `0x10000`; prefer the upstream command/web workflow once it
has been verified on the actual device.

## Host bridge

Install or run the Python 3.10+ bridge only with uv:

```sh
uv sync --project host
uv run --project host knietty --device auto
```

The default command is `tmux new-session -A -s knietty` when tmux is installed,
otherwise `$SHELL`. The PTY receives `TERM=vt100`, `COLUMNS=50`, `LINES=22`, and
the corresponding `TIOCSWINSZ` geometry. Disconnects retain the PTY/tmux client;
automatic discovery retries and sends `SIGWINCH` after reconnect so applications
redraw the same session.

Automatic discovery uses pyserial metadata. It prefers `/dev/cu.usbmodem*` on
macOS and `/dev/ttyACM*` on Linux, plus observed XTEINK/CrossPoint/knietty or
Espressif metadata. It rejects ties. Once the X4 descriptors are known, pass
explicit filters for safer matching:

```sh
uv run --project host knietty --list-devices
uv run --project host knietty --device auto --vid 0xVVVV --pid 0xPPPP \
  --serial-number DEVICE_SERIAL
```

## Linux user integration

Install the host command for the current user:

```sh
uv tool install ./host
```

Inspect the connected device before creating a udev rule:

```sh
lsusb
dmesg --follow
ls -l /dev/ttyACM*
udevadm info /dev/ttyACM0
```

Copy `host/integration/linux/99-knietty.rules.example`, replace every placeholder
with observed values, keep only one rule, and install it as
`/etc/udev/rules.d/99-knietty.rules`. Reload rules through the distribution's
normal administrative workflow. The rule adds `/dev/knietty` and `uaccess`; it
does not start a root daemon.

For a persistent per-user bridge:

```sh
mkdir -p ~/.config/systemd/user
cp host/integration/linux/knietty.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now knietty.service
```

This separation is intentional: udev provides access/stable naming, while the
already-running user service performs discovery and owns the user's tmux session.

## macOS user integration

Inspect the connected device first:

```sh
ls /dev/cu.usb*
ls /dev/tty.usb*
system_profiler SPUSBDataType
ioreg -p IOUSB -l -w 0
```

Install the command with `uv tool install ./host`. Copy
`host/integration/macos/dev.knietty.host.plist.in` to
`~/Library/LaunchAgents/dev.knietty.host.plist`, replacing
`__KNIETTY_EXECUTABLE__` with the absolute path reported by `command -v knietty`
and `__HOME__` with the absolute home directory. launchd does not expand shell
variables or `~` in plist strings.

Validate and load it as the current user:

```sh
plutil -lint ~/Library/LaunchAgents/dev.knietty.host.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.knietty.host.plist
```

No root LaunchDaemon, kernel extension, or DriverKit component is required.

## Validation matrix

Run the host unit tests on each OS and record the OS/version in
`TTY_PROGRESS.md`:

```sh
uv run --project host python -m unittest discover -s host/tests -v
```

Hardware validation must separately record first-prompt latency, output-to-panel
latency, button-to-PTY latency, actual refresh durations, ghosting, runtime free
heap, CPU/battery behavior, unplug/replug behavior, and tmux survival. Linux
results cannot be used to claim macOS parity or vice versa.
