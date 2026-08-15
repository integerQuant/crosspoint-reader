# knietty host bridge

The host bridge creates a 50 x 22 VT100 PTY, starts or attaches a persistent
tmux session when tmux is available, and forwards the PTY over the X4's USB CDC
port. Linux and macOS use the same POSIX implementation.

Install and run with `uv`:

```sh
uv sync --project host
uv run --project host knietty --device auto
```

An explicit device and command are also supported:

```sh
uv run --project host knietty \
  --device /dev/ttyACM0 \
  --cols 50 \
  --rows 22 \
  --command "tmux new-session -A -s knietty"
```

On macOS, use a `/dev/cu.usbmodem*` path rather than the corresponding
`/dev/tty.*` path for an explicit outbound connection. To inspect the metadata
that automatic matching sees:

```sh
uv run --project host knietty --list-devices
```

Until the actual X4 descriptors have been recorded, use `--vid`, `--pid`,
`--product`, or `--serial-number` if more than one candidate is present. The
bridge refuses equally ranked devices instead of silently attaching to the wrong
serial port.

PTY output is paced at 2048 bytes/second by default. This bounds output queued
during an e-paper refresh and applies normal PTY backpressure to noisy commands.
Override it with `--max-bps` only after measuring firmware RX reliability.

Run host tests:

```sh
uv run --project host python -m unittest discover -s host/tests -v
```
