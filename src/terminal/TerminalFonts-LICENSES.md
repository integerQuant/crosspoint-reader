# knietty terminal fonts

- Spleen 8x16 2.2.0 is Copyright (c) 2018-2026 Frederic Cambus and is
  distributed under the BSD 2-Clause license in `Spleen-LICENSE.txt`.
- Terminus Font 8x16 4.49.1 is Copyright (c) 2020 Dimitar Toshkov Zhekov,
  with Reserved Font Name "Terminus Font", and is distributed under the SIL
  Open Font License 1.1.
- GNU Unifont 8x16 16.0.04 is Copyright (c) 1998-2025 Roman Czyborra, Paul
  Hardy, Qianqian Fang, Andrew Miller, Johnnie Weaver, David Corbett, Nils
  Moskopp, Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, and other contributors.
  The generated subset is distributed under the SIL Open Font License 1.1.

The repository already includes the complete SIL Open Font License 1.1 text at
`lib/EpdFont/builtinFonts/source/NotoSans/OFL.txt`. Upstream sources:

- <https://github.com/fcambus/spleen/tree/2.2.0>
- <https://terminus-font.sourceforge.net/>
- <https://ftp.gnu.org/gnu/unifont/unifont-16.0.04/>

The base generated alternatives use the same codepoint manifest as the default
Spleen table. Terminus is generated from `ter-u16n.bdf`; the release table then
adds explicit agent-harness glyphs from `TerminalAgentGlyphs.txt` and bounded
native-width symbol ranges from `TerminalSymbolGlyphs.txt`. Missing explicit
glyphs come from GNU Unifont, with its 16-pixel symbols deliberately scaled into
the existing 8-pixel cell. Range additions accept only source glyphs no wider
than eight pixels. Ordinary double-width text still needs a future
`wcwidth`-aware screen model and remains deliberately excluded.

Source-file SHA-256 values used for this checkpoint:

```text
Terminus 4.49.1 archive  d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79
Unifont 16.0.04 BDF      40343cd7e33df7351c5a448497473697f82865e766c3ebb059f3ed6ade765587
```

Generation shape (replace the source paths with the downloaded files):

```sh
python3 scripts/generate_terminal_font.py ter-u16n.bdf \
  --subset-from src/terminal/TerminalFontData.generated.h \
  --add-codepoints-from src/terminal/TerminalAgentGlyphs.txt \
  --add-narrow-codepoints-from src/terminal/TerminalSymbolGlyphs.txt \
  --fallback-bdf unifont-16.0.04.bdf --downsample-wide-fallback \
  --max-glyphs 2048 --expected-glyph-count 2046 \
  --source-name "Terminus Font 8x16 4.49.1" \
  --fallback-source-name "GNU Unifont 16.0.04" \
  --license-note "Terminus and selected GNU Unifont glyphs; SIL Open Font License 1.1." \
  --output src/terminal/TerminalFontData.terminus.generated.h

python3 scripts/generate_terminal_font.py unifont-16.0.04.bdf \
  --subset-from src/terminal/TerminalFontData.generated.h --exclude-wide \
  --source-name "GNU Unifont 8x16 16.0.04" \
  --license-note "Copyright (c) Unifont contributors; SIL Open Font License 1.1." \
  --output src/terminal/TerminalFontData.unifont.generated.h

python3 scripts/generate_terminal_font_gallery.py
```
