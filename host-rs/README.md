# knietty Rust host

This package is the Linux/macOS knietty host. It discovers an X4 over Wi-Fi,
runs a shell or tmux session in a correctly sized PTY, bridges terminal traffic,
and runs the physically approved display-diagnostics suites.

The first migration slice provides:

- the complete bounded protocol v3 frame and diagnostics codec;
- self-contained golden protocol fixtures;
- repeated UDP LAN discovery with stable tabular output;
- the new `list` command and the existing `--list-devices` alias.

The second migration slice adds the portable PTY and terminal-safety
foundation: process-group isolation, controlling-TTY setup, window sizing,
`TERM`/`COLUMNS`/`LINES`, tmux/shell selection, escalating child cleanup, and
an RAII local-terminal guard that restores termios and descriptor flags during
normal return or panic unwinding.

The third slice adds the foreground Wi-Fi bridge: v3/v2/v1 negotiation,
physical-approval handling, explicit hosts and ambiguity rejection, bounded v3
framing, 64 KiB/s default pacing, nonblocking TCP/PTTY/local-keyboard polling,
disconnect and opt-in reconnect behavior, and signal-safe cleanup. Loopback
fake-device tests cover fallback, denial, malformed approval, bidirectional v3
traffic, disconnect, reconnect, and SIGTERM restoration. The foreground
non-daemon behavior matrix has been user-confirmed on macOS and Linux,
including discovery, approval/denial, PTY controls, disconnect/reconnect,
diagnostics, and post-diagnostics sleep/wake. Daemon supervision remains a
separate Linux backlog item.

On a deliberate terminal-mode exit, current firmware sends protocol v3
`SESSION_END`; the foreground bridge returns immediately without relying on a
TCP FIN surviving the X4's Wi-Fi teardown. TCP keepalive is configured as an
approximately six-second fallback for an abruptly powered-off or unreachable
X4. With `--reconnect`, either event returns the bridge to discovery instead of
ending the process.

The fourth slice ports the complete approved diagnostics client: smoke,
latency, cadence, and burst suites; response and refresh-event validation;
coalesced cadence ranges; uint32 timestamp ordering; interruptible deadlines;
and deterministic JSONL in the frozen diagnostics schema. A fake X4 completes a
full smoke campaign and cadence coalescing in the Rust test suite. The physical
suites were user-confirmed on both target operating systems.

The foreground bridge also exposes a private, per-device Unix control socket
after a protocol-v3 terminal is connected. A second CLI process can query the
active profile, request one safe HALF clean, or select explicit display
polarity without opening another X4 connection:

```sh
knietty display status
knietty display clean
knietty display polarity inverted
knietty display polarity normal
```

The command waits for the X4's READY refresh telemetry before a clean or
polarity change succeeds. If more than one local bridge is active, select the
discovery name with `--device knietty-xxxxxx`. The runtime directory is private
to the current user and each socket is mode `0600`; this local boundary does
not encrypt the existing X4 TCP stream. Raw driver controls are not exposed.

Install the release binary for the current user from the repository root:

```sh
cargo install --locked --path host-rs
```

This normally installs `knietty` under Cargo's user binary directory. Use
`command -v knietty` to obtain the absolute path for service templates.

Run discovery from the repository root:

```sh
cargo run --manifest-path host-rs/Cargo.toml -- list --discovery-timeout 3
```

The legacy spelling also works:

```sh
cargo run --manifest-path host-rs/Cargo.toml -- --list-devices --discovery-timeout 3
```

When the X4 is on knietty's waiting screen, start a foreground session with:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- \
  --host auto --verbose
```

The explicit command spelling is equivalent:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- \
  connect --host 192.168.1.42 --reconnect
```

Local input is enabled automatically when stdin is a terminal. Ctrl+C is sent
to the PTY; Ctrl+\\ exits locally and restores terminal state. Use
`--no-local-input` for a headless foreground process.

For a reproducible terminal-parser trace, explicitly capture the raw PTY bytes
sent to the X4:

```sh
mkdir -p captures
knietty --host auto --verbose \
  --capture-output captures/codex-session.raw
```

The capture is output-only, but screen contents and terminal-echoed input can
contain secrets. knietty creates a new mode-`0600` file, refuses to overwrite an
existing path, and stops the session at 8 MiB by default. Change the bound with
`--capture-limit BYTES`. The repository ignores `captures/`; review and redact
a trace before sharing it, and commit only synthetic replay fixtures.

Exercise the PTY layer without making a network connection:

```sh
cargo run --manifest-path host-rs/Cargo.toml -- \
  pty-smoke --cols 42 --rows 21 \
  --command 'printf "%s:%s:%s\\n" "$TERM" "$COLUMNS" "$LINES"'
```

Run one physically approved Rust diagnostic:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite smoke --output results/rust-smoke.jsonl
```

Run the complete Rust software matrix:

```sh
./host-rs/scripts/check.sh
```

See [`HOST_MATRIX.md`](HOST_MATRIX.md) for the macOS/Linux test procedure and
recorded result. Foreground service templates live under `integration/`; replace
the macOS placeholders with absolute paths before loading the LaunchAgent. The
Linux systemd user unit assumes the default Cargo install path. Daemon
supervision is deferred to the Linux integration backlog. No USB or Python
runtime dependency is included in the host.
