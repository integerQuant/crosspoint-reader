# Agent-harness terminal compatibility

## Status

The knietty 0.1.3 software candidate implements the bounded terminal work needed
by Codex, Claude Code, and OpenCode without attempting full xterm, color, or
Unicode compatibility. It keeps the validated 0.1.2 transport and display
scheduler unchanged. Native tests, Rust host checks, static analysis, and the
`knietty_async_window` firmware build pass; physical X4 validation remains.

## Objective

Render agentic terminal applications coherently at 99 x 28: correct scrolling
and editing, stable cursor placement, no control-string leakage, deterministic
capability negotiation, and useful monochrome symbols. Keep every parser data
structure statically bounded and preserve enough heap for Wi-Fi and TLS.

## Optional Codex profile

The following user-selected profile reduces unnecessary E Ink churn:

```toml
[tui]
animations = false
alternate_screen = "never"
terminal_title = []
notifications = false
```

Do not silently rewrite a user's Codex configuration. The firmware also handles
the alternate-screen path, so this profile is an optimization rather than a
correctness requirement.

## Implemented parser surface

- OSC, DCS, APC, and PM strings are bounded and consumed through BEL or ST.
- Cursor save/restore, margins, index/reverse-index, explicit scroll, cursor
  movement, and absolute row/column positioning are handled.
- Insert/delete/erase character and line operations and bounded REP are handled.
- Extended color SGR parameters are consumed as one unit and mapped to
  monochrome. Hidden and strikethrough attributes are supported.
- DEL and known zero-width selectors/joiners are consumed without taking a cell.
- DSR 5, CPR/DSR 6, DA1, XTVERSION, DECRQM, XTGETTCAP `Ms`, and OSC 10/11 receive
  small deterministic replies through the existing encrypted input channel.
- Unsupported Kitty keyboard, graphics, notification, focus, true-color, and
  explicit-width capabilities receive no false-positive reply. In particular,
  replying to `CSI ? u` would make OpenCode believe Kitty keyboard mode exists.
- DEC synchronized output (`?2026`) holds presentation until the boundary, with
  a 250 ms watchdog so a missing boundary cannot freeze the E Ink display.
- Alternate-screen entry and exit are idempotent and clear/reset the fixed
  terminal model. No third screen buffer or heap allocation is introduced.
- Session disconnect clears partial parser state and presentation holds without
  leaking terminal modes into the next connection.

G0/G1 designation is consumed safely, but DEC Special Graphics mapping remains
deferred because the captured Codex stream did not require it.

## Harness source audit

The local 0.1.3 audit covered Codex 0.149.0, Claude Code 2.1.241's classic
renderer, and OpenCode/OpenTUI source snapshots available during development.
OpenTUI probes XTVERSION, DSR/CPR, DA1, several DECRQM modes, Kitty keyboard,
XTGETTCAP `Ms`, OSC foreground/background colors, notifications, Kitty graphics,
and explicit-width behavior. knietty answers only the capabilities it actually
implements and reports synchronized-output state through DECRQM.

Claude Code and OpenCode both use ordinary alternate-screen/full-screen TUI
behavior. Clear-on-transition semantics were chosen over a second 99 x 28 cell
buffer because preserving the hidden main screen is not worth the ESP32-C3 heap
cost. The host PTY remains `TERM=vt100`; a custom terminfo entry stays deferred
until the capability set has passed hardware validation.

## Packed cells and glyph supplement

The release Terminus table contains 2,046 sorted glyphs. It combines the base
Terminus repertoire, explicit symbols observed in Codex/Claude Code/OpenCode,
and reviewed single-cell ranges for punctuation, arrows, mathematics, box and
block drawing, geometric symbols, dingbats, and Braille. Range imports accept
only native glyphs no wider than eight pixels; explicitly curated wide icons
may be adapted from 16 to 8 pixels. Two indices remain reserved under the 2,048
glyph hard limit.

Each stored screen cell is one 16-bit value: an 11-bit glyph-table index and all
five attribute bits. Codepoint lookup happens once when UTF-8 enters the screen,
not for every repaint. The two 99 x 28 screen models therefore consume 11,088
bytes together instead of 22,176, an exact permanent saving of 11,088 bytes.
The transient test/debug `Cell` view remains four bytes and does not back the
screen array. Rendering reads glyph rows directly by index, eliminating the
former per-cell binary search.

The private Codex capture contained only two misses against the earlier table:
U+2074 and U+21B5; both are now included. The generator requires exactly 2,046
glyphs and rejects a table above 2,048. Full Unicode, double-width characters,
combining, shaping, and Nerd Font private-use glyphs remain out of scope.

## Original Codex regression

The private capture is not committed. It established that OSC title text caused
the visible `0;hostname...` corruption, `CSI 0 SP q` leaked the phantom `q`, and
missing scrolling-margin/reverse-index handling hid the newest response. Those
failures were fixed and physically validated in the 0.1.2 lineage. Synthetic
native tests retain the control-sequence behavior without retaining private PTY
content.

## 0.1.3 acceptance gate

- Flash the exact 0.1.3 candidate by the established SD updater; do not use USB
  flashing on the locked X4.
- Exercise Codex, Claude Code, and OpenCode through approval, TLS connection,
  thinking/tool output, transcript scrolling, completion, and exit.
- Confirm alternate-screen entry/exit and synchronized repainting never expose
  an old frame or leave the display held.
- Confirm no control payload becomes printable text and each supplemented glyph
  renders instead of `?` for the tested corpus.
- Recheck Ctrl+C, deliberate terminal exit, reconnect, reader sleep/wake, and
  minimum heap. One physical host OS does not establish Linux/macOS parity.
