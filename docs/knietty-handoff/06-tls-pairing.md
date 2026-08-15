# Milestone 06 — TLS and persistent pairing

## Objective

Encrypt and authenticate v3 without changing its frame semantics. Preserve the
X4's physical approval model and support an unattended, already-paired daemon.

## Source anchors

- Plain TCP server: [`src/terminal/TerminalWifi.h`](../../src/terminal/TerminalWifi.h#L3)
- Connection lifecycle: [`src/terminal/TerminalWifi.cpp`](../../src/terminal/TerminalWifi.cpp#L32)
- wolfSSL build configuration: [`platformio.ini`](../../platformio.ini#L38)
- Existing outbound-only TLS wrapper:
  [`freeink-sdk/libs/network/SecureNet/include/SecureClient.h`](../../freeink-sdk/libs/network/SecureNet/include/SecureClient.h#L14)
- Existing TLS heap gate example:
  [`lib/KOReaderSync/KOReaderSyncClient.cpp`](../../lib/KOReaderSync/KOReaderSyncClient.cpp#L46)

## Threat model and locked requirements

Protect terminal contents and input from passive LAN observers and reject hosts
that were not paired. Pin a persistent per-device identity on the host and a
persistent host identity on the X4. Never ship one private key shared by every
firmware image, never use `setInsecure()` in release mode, and never label plain
TLS without identity verification as paired.

Initial pairing is interactive. Prefer a short authentication string derived
from the authenticated handshake transcript, displayed by both host and X4,
then confirmed physically. If the final implementation uses trusted-LAN TOFU
instead, label that weaker threat model explicitly and require the user to
confirm the device fingerprint once. Headless operation begins only after this
first pairing.

The X4 has no secure element and storage encryption has not been established;
document that persistent private material may not be protected against physical
SD/flash extraction.

## Architecture

The current `SecureClient` initiates outbound connections and cannot secure
`NetworkServer::accept()`. Add a narrowly scoped wolfSSL server-session wrapper
around an accepted `NetworkClient`, or move equivalent functionality into
`SecureNet`. Verify server APIs and compiled algorithms against the pinned
wolfSSL source before coding.

TLS wraps the accepted socket before the v3 greeting. Discovery may advertise a
non-secret identity fingerprint and `tls=required`, but discovery is not trusted
for authentication. After pairing, the Rust host verifies the pinned device and
the X4 verifies the pinned host credential. Plain v1/v2 is disabled by default
in secure builds; a plainly named development flag may re-enable it on trusted
networks.

## Resource plan

Measure free heap, minimum heap, largest allocatable block, handshake duration,
and steady-session heap before/after. Reuse the existing 2 KiB TLS-record strategy
where compatible. Do not run a TLS handshake concurrently with a display clean
or large temporary window allocation. Reject gracefully before allocation when
the measured heap gate is not met; never let `new` abort the firmware.

## Pairing lifecycle

- list and revoke paired hosts on-device;
- reject key changes for an existing host identity;
- explicit “forget all” action with confirmation;
- atomic credential write and recovery from interrupted write;
- no pairing material or session plaintext in logs/JSONL;
- rate-limit handshake and approval attempts.

## Verification gate

- Packet capture contains no terminal plaintext.
- Unknown host, wrong pin, expired/corrupt credential, replayed application
  frame, truncated TLS record, and key-change attempts fail closed.
- Paired reconnect works after host/device restart and preserves tmux behavior.
- Diagnostics still distinguishes `PRESENTED` and `READY`; TLS overhead is
  measured separately from display latency.
- Sleep/wake, exit, normal OTA recovery, free heap, and idle daemon CPU remain
  acceptable on the physical X4, macOS, and Linux.

## Complete when

A fresh pair requires human verification and a physical X4 confirmation;
subsequent paired reconnect is unattended and mutually authenticated; captures
show no plaintext; and resource measurements are recorded without regression.
