# knietty host bridge

The host bridge creates an 80 x 24 VT100 PTY, starts or attaches a persistent
tmux session when tmux is available, discovers the X4 with a LAN UDP probe, and
forwards the PTY over a low-latency TCP connection. The X4 also advertises the
standard `_knietty._tcp.local` mDNS service. Linux and macOS use the same POSIX
implementation.

Install and run with `uv`:

```sh
uv sync --project host
uv run --project host knietty --host auto
```

Run the bounded display smoke suite without creating a PTY:

```sh
uv run --project host --no-sync \
  knietty diagnose --host auto --suite smoke --output results/run.jsonl
```

The X4 shows a distinct display-diagnostics request. Confirm allows only the
compiled named test patterns; Back, Power, host disconnect, inactivity, and
session limits abort the run. The JSON Lines file is flushed after every record
and remains readable through the last complete line if interrupted.

An explicit IP address and command are also supported:

```sh
uv run --project host knietty \
  --host 192.168.1.42 \
  --cols 80 \
  --rows 24 \
  --command "tmux new-session -A -s knietty"
```

Open knietty on the X4 first. It uses CrossPoint's saved-network selection,
advertises `_knietty._tcp.local`, answers discovery probes, and displays the
requesting host name and IP.
The approval screen labels the X4's physical Accept and Deny buttons. To inspect LAN discovery without
starting a PTY:

```sh
uv run --project host knietty --list-devices
```

The bridge refuses ambiguous discovery instead of silently choosing the wrong
X4. Pass `--host IP_ADDRESS` when more than one terminal is active.

When run from an interactive terminal, the bridge automatically forwards that
terminal's keyboard into the remote PTY; shell echo and output appear on the X4.
Ctrl+C is sent to the remote shell's PTY process group; it does not signal the
bridge. Press Ctrl+\\ to stop the bridge cleanly, including while discovery or
approval is being retried.
Pass `--no-local-input` for daemon-like behavior
from an interactive terminal. systemd and launchd sessions have no TTY, so local
input stays disabled automatically.

After an established terminal disconnects, the default interactive behavior is
to close the bridge and restore the local terminal. Pass `--reconnect` for a
long-running daemon that should resume discovery after disconnect. Repeated
discovery errors are rate-limited so a disconnected daemon does not flood its
log.

PTY output is paced at 65536 bytes/second by default. This bounds output queued
during an E Ink refresh and applies normal PTY backpressure to noisy commands.
Override it with `--max-bps` after measuring firmware RX reliability.

Protocol v3 carries terminal bytes in bounded frames and sends the host's
wall-clock time and UTC offset for the X4 header. Auto mode tries v3, then falls
back to raw-stream v2 and v1 firmware. Pass `--protocol 2` to force a
compatibility test. The protocol is unencrypted and relies on explicit approval
on the X4. Treat it as trusted-LAN-only. Authentication and encryption are
follow-up work, not properties of this checkpoint.

The byte-level v3 contract is documented in
[`docs/knietty-protocol-v3.md`](../docs/knietty-protocol-v3.md).

USB CDC remains available only as a legacy host option:

```sh
uv run --project host knietty --transport usb --device /dev/ttyACM0
```

On macOS, prefer `/dev/cu.usbmodem*` for an explicit USB connection.

Run host tests:

```sh
uv run --project host python -m unittest discover -s host/tests -v
```
