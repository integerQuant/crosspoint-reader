<div align="center">

# knietty

### A real terminal for your E Ink reader.

Turn an **XTEINK X4** into an encrypted, wireless **80 x 24 terminal** for
shells, tmux, SSH, development tools, and text-first TUIs—without giving up the
reader firmware underneath it.

**EINK backwards → KNIE + TTY**

[![Latest release](https://img.shields.io/github/v/release/integerQuant/crosspoint-reader?filter=knietty-v*&display_name=release&style=flat-square&label=knietty)](https://github.com/integerQuant/crosspoint-reader/releases/latest)
[![Host](https://img.shields.io/badge/host-macOS%20%7C%20Linux-111?style=flat-square)](#host-support)
[![Transport](https://img.shields.io/badge/transport-TLS%201.3%20over%20Wi--Fi-111?style=flat-square)](#secure-by-default)
[![License](https://img.shields.io/badge/license-MIT-111?style=flat-square)](LICENSE)

[Install](#quick-start) · [How it works](#how-it-works) · [Commands](#useful-commands) · [Development](#development) · [Credits](#lineage-and-credits)

</div>

---

knietty is a focused fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). It
adds a terminal Activity to the existing reader, a portable Rust host bridge,
LAN discovery, first-pair approval, persistent TLS identities, a compact VT
screen model, and an X4-specific fast-refresh pipeline.

Open Terminal from the CrossPoint menu, connect from your computer, and your
shell appears on the E Ink panel. Exit Terminal and the X4 returns to being a
reader.

## What you get

- **80 x 24 fixed-cell terminal** using Terminus, with ANSI/VT behavior tuned
  for shells, tmux, Codex, and full-screen TUIs such as btop.
- **Responsive E Ink updates** through dirty-region rendering, burst-aware
  batching, adaptive SSD1677 waveforms, and asynchronous display work.
- **Zero-configuration discovery** on a shared local network—no fixed IP
  address or USB serial device required.
- **TLS 1.3 by default.** The host and X4 display the same six-digit code on
  first pairing, then pin each other's identity for later reconnects.
- **Portable Rust host** with PTY sizing, tmux/shell fallback, clean signal
  handling, reconnect support, and no Python runtime.
- **Reader-safe integration.** Terminal is an ordinary CrossPoint Activity;
  deliberate exit returns to the menu and normal reader sleep behavior.
- **Observable display pipeline** with safe clean, polarity, status, metrics,
  and physically approved bounded diagnostic commands.

## Quick start

### 1. Install the firmware

> [!CAUTION]
> knietty firmware currently targets the **XTEINK X4**. Keep a known-good
> official CrossPoint image and verify your recovery path before installing
> experimental firmware. Do not alter the bootloader, partitions, eFuses, or
> secure-boot state.

Download `knietty-0.1.2.bin` and its adjacent `.sha256` file from the
[latest release](https://github.com/integerQuant/crosspoint-reader/releases/latest),
verify the checksum, copy the application image to the SD card, and select it
through CrossPoint's normal in-application firmware updater.

For a USB-locked X4, start from a working official CrossPoint installation and
use that SD update path. Do **not** use `esptool` or the Xteink Unlocker to flash
knietty. The release asset is application firmware, not a complete flash image.

### 2. Install the host

The installer downloads the matching release archive, verifies its SHA-256
checksum, and installs `knietty` to `~/.local/bin`. It does not use `sudo`, edit
your shell configuration, run a daemon, or flash the X4.

```sh
curl -fsSL https://rmtb.dev/knietty | sh
```

Install a specific release when reproducibility matters:

```sh
curl -fsSL https://rmtb.dev/knietty | sh -s -- --version 0.1.2
```

Make sure `~/.local/bin` is on `PATH`, then verify the install:

```sh
knietty --version
```

### 3. Connect

Connect the computer and X4 to the same Wi-Fi network, open **Terminal** on the
X4, and run:

```sh
knietty list
knietty --host auto
```

On the first connection, compare the six-digit code printed by the host with
the code on the X4. Press Confirm only when they match. knietty creates or
attaches a tmux session when tmux is available and otherwise starts your shell.

`Ctrl+C` is forwarded to the remote PTY. `Ctrl+\` exits the local bridge and
restores the host terminal.

## Useful commands

| Command | Purpose |
| --- | --- |
| `knietty list` | Discover X4 terminals on the LAN |
| `knietty --host auto` | Connect to the only discovered terminal |
| `knietty --host auto --reconnect` | Return to discovery after a disconnect |
| `knietty display status` | Read firmware, network, geometry, and profile state |
| `knietty display metrics --json` | Read host and display-pipeline counters |
| `knietty display clean` | Schedule one safe ghost-cleaning refresh |
| `knietty display polarity inverted` | Switch to light-on-dark terminal rendering |
| `knietty display polarity normal` | Return to dark-on-light rendering |
| `knietty diagnose --host auto --suite smoke` | Run a bounded, physically approved display test |

Display controls operate through the already connected foreground bridge. They
do not expose raw controller registers or unsafe voltage, OTP, or arbitrary
clock controls.

## How it works

```text
shell / tmux
     ↕ PTY
knietty Rust host
     ↕ TLS 1.3 over Wi-Fi
XTEINK X4
     ↕ terminal parser + 80 x 24 cell model
CrossPoint renderer
     ↕ dirty windows + adaptive waveform
SSD1677 E Ink panel
```

The host sends terminal output in bounded frames and marks logical burst
boundaries. Firmware parses those bytes while display work is in flight,
coalesces changes into the newest screen state, redraws only affected regions,
and periodically performs stronger maintenance updates to contain ghosting.

This is still E Ink: it excels at code, shells, logs, dashboards, and deliberate
interaction—not animation or video. Large TUI repaints are intentionally
batched into coherent updates.

## Secure by default

Terminal traffic and approval messages use mutually authenticated TLS 1.3.
Each installation creates a persistent P-256 identity. The first physical
approval pins the host on the X4 and the device on the host; future connections
from that pair can reconnect without repeating approval.

Discovery remains plaintext and advertises only presence, addressing, and the
TLS requirement. Private host state is stored with owner-only permissions in:

- macOS: `~/Library/Application Support/knietty/`
- Linux: `${XDG_CONFIG_HOME:-~/.config}/knietty/`

The X4 has no secure element, so physical flash extraction is outside the
project's threat model.

## Host support

Release binaries currently cover:

| Host | Artifact |
| --- | --- |
| Apple silicon macOS | `aarch64-apple-darwin` |
| x86-64 Linux | `x86_64-unknown-linux-gnu` |
| x86-64 Arch Linux | native Arch build |

Intel macOS, Linux ARM, and musl Linux do not yet have release artifacts. The
host can still be built from source where Rust and its dependencies support the
platform.

## Development

Clone the firmware and its knietty display-driver fork together:

```sh
git clone --recursive https://github.com/integerQuant/crosspoint-reader.git
cd crosspoint-reader
```

Run the complete Rust host gate:

```sh
./host-rs/scripts/check.sh
```

Build the hardware-tested firmware profile:

```sh
pio run -e knietty_async_window
```

Build all release artifacts locally, including native, Ubuntu, Arch, and X4
firmware gates:

```sh
scripts/build-knietty-release-local.sh
```

More detail lives in the [knietty technical guide](docs/knietty.md), the
[Rust host guide](host-rs/README.md), and the
[milestone handoff](docs/knietty-handoff/README.md).

## Project status

The current release is **knietty 0.1.2**. The X4 firmware and Apple silicon
macOS host have been exercised together on physical hardware. Linux and Arch
release artifacts pass their native software matrices; do not infer hardware
parity from the macOS/X4 test.

BLE keyboard input is not included in 0.1.2. Early experiments could not retain
a safe contiguous-heap margin alongside Wi-Fi, TLS, CrossPoint, and the terminal
framebuffer, so BLE experiment images are deliberately excluded from releases.

## Lineage and credits

knietty stands on a substantial open-source foundation:

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
  created by Dave Allie and developed by its contributors, provides the reader,
  Activity/navigation model, rendering stack, storage, networking, updater,
  power management, and most of the firmware around Terminal.
- [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) provides the hardware
  and display abstraction. The [knietty FreeInk fork](https://github.com/integerQuant/freeink-sdk)
  carries the X4 refresh-pipeline changes used here.
- FreeInk's X4 panel work descends from the MIT-licensed
  [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk),
  including SSD1677 driver and waveform work by CidVonHighwind and the wider
  OpenX4 community.
- The default terminal face is
  [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov
  Zhekov, distributed under the SIL Open Font License. Complete font notices
  are preserved in [TerminalFonts-LICENSES.md](src/terminal/TerminalFonts-LICENSES.md).

Please support and contribute fixes upstream when they belong there. This fork
is not affiliated with Xteink or any device manufacturer.

CrossPoint, FreeInk, and knietty are distributed under the [MIT License](LICENSE).
