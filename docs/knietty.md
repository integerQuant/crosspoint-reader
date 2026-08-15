# knietty terminal mode

knietty is an opt-in CrossPoint build plus a user-space host bridge. It retains
the reader firmware and activity lifecycle, uses CrossPoint's saved Wi-Fi
configuration, and renders a bounded character-cell screen into the existing
E Ink framebuffer. USB CDC remains available as a legacy transport, but no CDC
device was exposed by the tested China-locked X4.

Nothing in this document is evidence of physical X4 validation. Check
`TTY_PROGRESS.md` for the exact tested state before flashing.

## Firmware architecture

Build environment `knietty` adds `KNIETTY_ENABLED` and
`KNIETTY_STABLE_POWER`, and deliberately omits `ENABLE_SERIAL_LOG`. The stable
power flag retains CrossPoint 1.5.0's deep-sleep resume semantics and disables
the newer development branch's experimental light-sleep paths for this build.
Terminal mode itself prevents auto-sleep and owns the Power button; after
leaving Terminal, normal CrossPoint sleep behavior resumes.

The Home menu opens `TerminalActivity`, which selects a saved network and then
switches the renderer to native 800 x 480 landscape. It owns:

- a fixed 80 x 24 screen of four-byte Unicode cells (7,680 bytes per model);
- a small VT100-style parser with bounded parameters and incremental UTF-8;
- a generated 1,001-glyph Spleen 8 x 16 bitmap table stored in flash;
- 10 x 18 cells that use all 800 horizontal pixels with a one-pixel internal
  glyph guard;
- dirty row spans and a render snapshot so network RX continues while the
  E Ink waveform runs;
- Wi-Fi discovery/approval/stream transport and logical CrossPoint button
  input.

RX is drained continuously while the render task waits for the panel. Parser
mutations occur behind a short model lock; rendering copies a stable snapshot
and releases that lock before drawing or refreshing. Output bursts wait 8 ms
after the latest byte or 20 ms from the first byte, whichever happens first.
Only the changed column span of each dirty row is redrawn. Normal updates use
the X4 SSD1677 byte-aligned differential-window path when the temporary transfer
is at most 8 KiB; larger or unsupported regions safely fall back to the resident
whole framebuffer. A HALF clean is used on entry and after 50 fast updates.
These constants and source waveform timings are not hardware measurements.

Supported input is BMP UTF-8 with a replacement glyph for invalid/non-BMP
input, LF, CR, backspace, tab, BEL (ignored), delayed line wrap, scroll, cursor
movement, screen/line erase, cursor visibility, and SGR
normal/bold/inverse/underline. ANSI colors are accepted and ignored. Spleen
adds Latin, Greek, Cyrillic, box drawing, block elements, Braille, and a small
Powerline subset; this is a compact terminal font, not a complete Nerd Font.

Controls are:

| Logical button | PTY input |
| --- | --- |
| Up / Down / Right / Left | `ESC [ A/B/C/D` |
| Confirm | carriage return |
| Back | Escape |
| Confirm held at least 1 second | Ctrl+C |
| Back held at least 1 second | toggle black/white polarity |
| Power, then Power within 3 seconds | leave terminal mode |

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

## Locked-unit update and recovery

The tested X4 is China-locked and exposes no application CDC device. Do not use
PlatformIO upload, esptool, or alter its partition table, bootloader, secure-boot
state, or eFuses. Keep the known-good CrossPoint 1.4.1 application image and a
copy of every tested knietty image outside the build directory.

CrossPoint's normal network OTA has physically restored official 1.5.0 without
the Unlocker, establishing the recovery route for this unit. From that clean
base, an SD-menu installation of the SD-safe knietty image also completed and
booted. Use the same in-application SD update path for subsequent knietty
application images, and retain the official OTA route as recovery. No partition
changes are part of knietty.

The exact current artifact name, checksum, observed updater behavior, and next
hardware test are recorded in `TTY_PROGRESS.md`; that file takes precedence
over generic flashing instructions.

## Host bridge

Install or run the Python 3.10+ bridge only with uv:

```sh
uv sync --project host
uv run --project host knietty --host auto
```

The default command is `tmux new-session -A -s knietty` when tmux is installed,
otherwise `$SHELL`. The PTY receives `TERM=vt100`, `COLUMNS=80`, `LINES=24`, and
the corresponding `TIOCSWINSZ` geometry. Ctrl+C read from the local terminal is
written to the PTY and signals only the PTY child process group. Ctrl+\\ exits
the bridge even during retry waits. An established disconnect exits by default;
pass `--reconnect` for systemd/launchd operation so discovery resumes and the
tmux session can reattach.

Wi-Fi automatic discovery uses a bounded UDP broadcast probe; the X4 also
advertises `_knietty._tcp.local`. It rejects ties. Use an explicit address when
the network contains more than one terminal:

```sh
uv run --project host knietty --list-devices
uv run --project host knietty --host 192.168.1.42
```

The serial metadata filters and `/dev/cu.usbmodem*`/`/dev/ttyACM*` discovery
remain available with `--transport usb`.

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
