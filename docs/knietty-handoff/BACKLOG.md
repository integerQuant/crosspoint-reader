# knietty backlog after the terminal release

These items are intentionally outside the ordered terminal milestones.

## BLE keyboard input — rejected on the current architecture

The isolated BLE HID experiment is complete and must not be resumed as an
ordinary feature pass. Scanning and HID probing worked, and an MX Keys keyboard
could be found, paired, and connected. The combined ESP32-C3 Wi-Fi, TLS, BLE,
display, and CrossPoint workload did not retain a viable contiguous heap:

- BLE initialization reduced the reported largest block from roughly 114 KiB
  to under 8 KiB.
- Later terminal/TLS operation reached roughly 8 KiB free with a 2.9 KiB
  largest block and a 736-byte observed minimum.
- Iterations failed TLS/status allocation, froze before rendering, returned to
  the menu, broke Wi-Fi scanning, or panicked during Wi-Fi/BLE transitions.
- Trimming allocations moved the failure rather than producing a stable
  concurrent terminal.

The experiment was rolled back and knietty 0.1.2 remained the known-good
non-BLE baseline. Retry only with a materially different design, such as an
external BLE coprocessor/receiver, a replacement network/TLS architecture with
a proven contiguous-heap budget, or hardware with PSRAM. A retry requires an
isolated branch, allocation instrumentation, and explicit Wi-Fi/TLS/display
regression gates before HID behavior.

## Other deferred work

- Broad Nerd Font coverage requires `wcwidth`, double-width cells, combining
  behavior, and a flash/RAM budget; it is not merely another bitmap table.
- Custom terminfo should follow a stable, hardware-tested capability set.
- Persistent/headless daemon supervision remains deferred; the foreground Rust
  host and reconnect loop are the supported path.
- USB CDC may be revisited only on hardware that exposes an application CDC
  node; it remains absent on the tested locked X4.
- Optical instrumentation with a photodiode can replace high-speed-video timing
  if a safe, non-invasive setup becomes available.
