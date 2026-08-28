# knietty 0.1.3

knietty 0.1.3 makes the X4 terminal denser, friendlier to modern coding agents,
and safer on its tight ESP32-C3 memory budget.

## Highlights

- Native 8 x 16 Terminus cells expand the terminal from 80 x 24 to **99 x 28**.
- The fixed font table grows to **2,046 glyphs**, adding the box drawing, block,
  Braille, arrow, mathematical, geometric, and symbol coverage used by Codex,
  Claude Code, OpenCode, shells, and TUIs.
- VT parsing now handles insert/delete/erase operations, extended SGR, hidden
  and strikethrough attributes, terminal capability replies, zero-width
  selectors/joiners, synchronized output, and alternate-screen transitions.
- Packed 16-bit cells halve the active/presented screen-model footprint, while
  deferred allocation keeps both models off the critical Wi-Fi/TLS startup
  path.
- Wi-Fi and discovery memory were tightened without shrinking the accepted
  dynamic network pools or the 8 KiB asynchronous display staging buffer.
- New `display heap` and `display monitor` commands expose fixed-size,
  low-overhead device heap snapshots through the authenticated control channel.
- The repository-root `show-knietty-glyphs` utility renders the exact bundled
  font atlas on a connected X4.

## Validation

The release firmware passed the available XTEINK X4 plus Apple silicon macOS
hardware gate: 99 x 28 rendering, complete glyph atlas, saved-Wi-Fi cold boot,
remembered-host TLS, loaded btop traffic, full-screen fallback, clean refresh,
exit, discovery, and re-entry. The reproducible loaded minimum was 25,648 bytes
free, and every tested path recovered without a retained leak.

The macOS, generic x86-64 Linux, and Arch x86-64 host artifacts pass their local
software matrices. Linux artifacts have not been physically validated with the
X4, so this release does not claim Linux hardware parity.

BLE keyboard support remains excluded: the ESP32-C3 coexistence experiment did
not retain a safe contiguous-heap margin beside Wi-Fi, TLS, CrossPoint, and the
terminal framebuffer.

## Install

Install the user-space host without `sudo`:

```sh
curl -fsSL https://rmtb.dev/knietty | sh
```

Firmware remains a separate, manual SD-card application update. Download
`knietty-0.1.3.bin` and its adjacent checksum from this release, retain a
known-good official CrossPoint recovery image, and do not treat the firmware
asset as a complete flash image.
