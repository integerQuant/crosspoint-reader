# Milestone 05 — Rust host migration

## Objective

Implement the Linux/macOS host bridge in Rust with protocol, PTY, signal,
terminal-restoration, reconnect, discovery, diagnostics, and JSONL parity. Keep
the Python/uv host installed and usable until both operating systems pass.

## Source anchors

- Python behavioral reference: [`host/knietty.py`](../../host/knietty.py)
- Existing parity tests: [`host/tests/test_knietty.py`](../../host/tests/test_knietty.py)
- Python package contract: [`host/pyproject.toml`](../../host/pyproject.toml)
- Linux integration: [`host/integration/linux`](../../host/integration/linux)
- macOS integration: [`host/integration/macos`](../../host/integration/macos)
- Protocol v3 specification produced by Milestone 02.

## Architecture choice

Use one small synchronous Unix event loop around `poll`/equivalent. The workload
has a TCP socket, PTY master, and local terminal descriptor; a general async
runtime adds complexity without improving panel latency. Isolate these modules:

```text
cli | discovery | protocol | transport | pty | terminal_guard | diagnostics
```

Use safe crates for CLI, signals, and Unix APIs where they materially reduce
unsafe code, but verify current upstream versions and macOS/Linux support at
implementation time. Keep all direct `unsafe` calls in the PTY/terminal module,
document invariants, and test cleanup on every error path. Do not add a serial
dependency unless USB support is intentionally retained and tested.

## Behavioral contract

- Same CLI defaults, 80 x 24 PTY geometry, `TERM=vt100`, tmux preference, shell
  fallback, client-name rules, discovery ambiguity rejection, and explicit IP.
- Local Ctrl+C goes to the PTY child process group; it never kills the bridge.
- Local Ctrl+\\ restores terminal state and exits, including retry waits.
- Established disconnect exits by default; `--reconnect` rediscovers and keeps
  the tmux session model.
- Child exit, signal, panic, I/O error, and protocol error restore termios and
  reap the child. Add a panic hook only as a final restoration fallback.
- Diagnostics output is byte/schema compatible with Python for golden sessions.

## Implementation order

1. Create a separate Rust package under `host-rs/`; do not overwrite Python.
2. Port v3 codec/golden vectors and discovery.
3. Port PTY spawn/window/environment and terminal RAII guard.
4. Port bridge/select loop, signals, child groups, and reconnect.
5. Port diagnostics and deterministic JSONL serialization.
6. Add cross-language fixture tests: Python encode → Rust decode and vice versa.
7. Run the same fake-device integration scenarios against both binaries.
8. Switch integration templates only after real Linux and macOS parity.

## Verification gate

- `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, and tests.
- Python/uv tests remain green.
- Leak/file-descriptor checks where available on each OS.
- Forced denial, malformed frame, socket reset, PTY child exit, SIGTERM,
  Ctrl+C, Ctrl+\\, and reconnect restore the local terminal.
- macOS and Linux each run discovery, shell, tmux, diagnostics smoke, and
  disconnect. Record OS/version separately.

## Complete when

Rust passes the documented behavior matrix on both target operating systems and
Python remains a working oracle/fallback. Do not delete Python in this milestone.
