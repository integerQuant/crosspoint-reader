# knietty host bridge

The host bridge creates a 50 x 22 VT100 PTY, starts or attaches a persistent
tmux session when tmux is available, discovers the X4 with a LAN UDP probe, and
forwards the PTY over a low-latency TCP connection. The X4 also advertises the
standard `_knietty._tcp.local` mDNS service. Linux and macOS use the same POSIX
implementation.

Install and run with `uv`:

```sh
uv sync --project host
uv run --project host knietty --host auto
```

An explicit IP address and command are also supported:

```sh
uv run --project host knietty \
  --host 192.168.1.42 \
  --cols 50 \
  --rows 22 \
  --command "tmux new-session -A -s knietty"
```

Open knietty on the X4 first. It uses CrossPoint's saved-network selection,
advertises `_knietty._tcp.local`, answers discovery probes, and displays the
requesting host name and IP.
Press Confirm to accept or Back to deny. To inspect LAN discovery without
starting a PTY:

```sh
uv run --project host knietty --list-devices
```

The bridge refuses ambiguous discovery instead of silently choosing the wrong
X4. Pass `--host IP_ADDRESS` when more than one terminal is active.

When run from an interactive terminal, the bridge automatically forwards that
terminal's keyboard into the remote PTY; shell echo and output appear on the X4.
Press Ctrl+C to stop the bridge. Pass `--no-local-input` for daemon-like behavior
from an interactive terminal. systemd and launchd sessions have no TTY, so local
input stays disabled automatically.

PTY output is paced at 16384 bytes/second by default. This bounds output queued
during an E Ink refresh and applies normal PTY backpressure to noisy commands.
Override it with `--max-bps` after measuring firmware RX reliability.

The prototype protocol is unencrypted and relies on explicit approval on the
X4. Treat it as trusted-LAN-only. Authentication and encryption are follow-up
work, not properties of this checkpoint.

USB CDC remains available only as a legacy host option:

```sh
uv run --project host knietty --transport usb --device /dev/ttyACM0
```

On macOS, prefer `/dev/cu.usbmodem*` for an explicit USB connection.

Run host tests:

```sh
uv run --project host python -m unittest discover -s host/tests -v
```
