# CJK rendering on the T-Deck Plus (320x240)

Question: how do we show Chinese/Japanese/Korean text on a 320x240 panel with
3.875 MB app slots and no font engine to spare? Short answer: precompile a
bitmap font with LVGL's own converter, choose coverage by frequency, and keep
the glyphs in flash (XIP). Don't ship FreeType or librime.

## The screen budget

| font size | chars per line (320 px) | usable lines (240 px, minus bars) |
|---|---|---|
| 12 px | 26 | ~16 |
| 14 px | 22 | ~13 |
| 16 px | 20 | ~12 |
| 24 px | 13 | ~8 |

So 16 px is the largest comfortable body size; 12 px works for lists and
candidate rows. CJK glyphs are full-width, so this is also the wrap width the
text fields should use.

## The flash budget

Current T-Deck table (`variants/lilygo_tdeck/partitions_tdeck_touch.csv`):

```
app0   7.75 MB   (firmware ~3.9 MB -> ~3.9 MB free, USB-only updates)
tiles  4.75 MB   (map tiles, unchanged)
spiffs 3.375 MB  (user data, unchanged)
```

See `firmware/tdeck-plus/partitions_tdeck_browser.csv`: the app slot doubled
when the OTA twin slot was dropped, so a full CJK bitmap font (below) now fits
in flash. Keeping it on the card is still worth it if the font should be
swappable without a reflash — see "Fonts on the SD card" below.

A CJK glyph at 16 px costs:

| format | bytes/glyph | 6,763 glyphs (GB2312) | 20,992 glyphs (U+4E00–U+9FFF) |
|---|---|---|---|
| 16 px, 1 bpp | 32 | ~216 KB + ~80 KB descriptors | ~656 KB + ~250 KB |
| 16 px, 2 bpp | 64 | ~432 KB + desc. | ~1.3 MB + desc. |
| 16 px, 4 bpp | 128 | ~865 KB + desc. | ~2.7 MB + desc. |
| 12 px, 1 bpp | 18 | ~122 KB + desc. | ~370 KB + desc. |

The descriptors (~12 B per glyph: advance, box, offsets, bitmap index) are what
people forget; they add roughly 25–35 % on top of the bitmap bytes.

Recommendation, in order of preference:

1. **16 px, 2 bpp, GB2312 coverage (6,763 glyphs), RLE-compressed** — roughly
   400–500 KB in flash. Antialiased, fits the ~1.3 MB headroom with room for
   growth, covers virtually all modern simplified Chinese plus the common
   punctuation/kana set.
2. **16 px, 1 bpp full CJK (U+4E00–U+9FFF)** — ~900 KB. Monochrome but complete;
   noticeably rougher at small sizes.
3. **12 px, 1 bpp full CJK** — ~630 KB for lists and candidate bars, 16 px
   reduced set for body text.

Do not use 4 bpp full CJK: ~3 MB does not fit beside a 2.55 MB firmware in one
OTA slot, and dropping OTA/tiles to make room is a worse trade than 2 bpp.

RAM: LVGL 8 fonts are read from flash, so the font data costs no SRAM (it is
mapped `.rodata`). Only the decoder's small working buffers are on the stack;
compressed fonts decode per draw. Keep the text buffer sizes as they are.

## Generated font, not runtime rasterizer

* FreeType on the S3 needs a scalable font in PSRAM, a rasterizer, hinting
  tables and a glyph cache; it is tens of KB of code and hundreds of KB of RAM
  for a result the 1–2 bpp bitmap gives for free from flash.
* LVGL ships `lv_font_conv`, which emits exactly the `lv_font_t` format
  wadamesh already uses for its 12/14/20 px fonts. wadamesh regenerates its
  fonts from `scripts/build/gen-touch-fonts.sh`; the CJK font belongs in the
  same step (a build-time asset, not source).

Recipe (run once, commit the generated `.c` or fetch it as a build asset):

```bash
npx lv_font_conv \
  --font NotoSansSC-Regular.otf \
  --range 0x20-0x7E --range 0x3000-0x30FF --range 0x4E00-0x9FFF \
  --size 16 --bpp 2 --compress --no-prefilter \
  --format lvgl --lv-include lvgl.h \
  -o components/fonts/cjk_16.c
```

Then:

* add the file to the T-Deck env (`build_src_filter` already climbs out of the
  submodule for the Kiwix app, so the font can live in this repository),
* set it as the **fallback** of the existing 14/16 px UI font
  (`--fallback` when generating, or `font->fallback = &cjk_16` at init), so
  every label, textarea and list already picks it up without touching screen
  code,
* keep latin glyphs in the existing font so the CJK font only carries the
  ranges it needs (Latin at 16 px in Noto would be redundant).

## Input is a separate problem

### lvgl_pinyin_ime

`third_party/lvgl_pinyin_ime` is pinned (1e2aa50) as the input-method
candidate. Its widget is an **LVGL 9.5 / ESP-IDF 6.1** component and this
firmware is Arduino/PlatformIO on **LVGL 8.3.11**, so the widget — keyboards,
candidate UI, embedded font arrays — does not compile here and is not wired in.

What is portable is the engine it vendors: libgooglepinyin (Apache-2.0, under
`src/third_party/libgooglepinyin`) is plain C++ with a C API
(`im_open_decoder` / `im_search` / `im_get_candidate`). `scripts/pinyin-dict.sh`
builds its dictionary builder with the host compiler and turns the pinned
`data/dict/rawdict_utf16_65105_freq.txt` + `valid_utf16.txt` into
`out/dict_pinyin.dat` — a 1,068,442-byte card asset for
`/sd/ime/dict_pinyin.dat`, not firmware. librime is not used at all: it needs
Boost/leveldb/marisa/OpenCC and has no ESP32 port.

Still missing for a working IME: an LVGL 8.3 candidate/keyboard UI, and CJK
glyphs to draw the candidates with (next section). Until both exist there is no
CJK input in this firmware, and no code here pretends otherwise.

Rendering CJK does not give you a way to type it, so CJK input needs a small
table IME:

* pinyin syllable table + a frequency-ordered dictionary on the SD card
  (`/sd/ime/pinyin.dict`, ~1–3 MB), memory-mapped or streamed;
* an editorial buffer + candidate list as an LVGL overlay above the active
  textarea, driven by the existing keyboard/trackball input groups;
* a "commit" path that inserts UTF-8 into the focused textarea.

That is a real app-sized feature; the font work above is the prerequisite and
the part that fits the current firmware without new partitions.

## Fonts on the SD card

LVGL 8.3 has no file-backed font driver: a `lv_font_t` is a static descriptor
plus bitmap arrays, read through `get_glyph_dsc`/`get_glyph_bitmap`. Keeping the
font off flash therefore means a small custom font object whose two callbacks
seek a glyph record in `/sd/fonts/cjk16.bin` (an index of offsets + 1–2 bpp
bitmaps generated by `lv_font_conv`'s data, or the same data re-packed) instead
of returning pointers into `.rodata`. That is a few hundred lines and a cache,
and SD reads are slower than XIP, so the first draw of each glyph costs a card
read; caching one screen's worth of glyphs is enough.

The trade is explicit: flash costs ~630–900 KB of the 7.75 MB app slot (free
now), while SD costs no flash, a per-glyph read, and the font disappears if the
card is removed — labels would then fall back to the built-in font and CJK
would show as missing glyphs. Nothing is committed for this yet: no generated
font asset and no SD font driver exist in this checkout.

## What is verified

The size arithmetic above is from the glyph geometry and the existing partition
table. The font is **not generated or compiled** in this checkout (no
`lv_font_conv`/font asset and no ESP32 toolchain here), so treat the exact
kilobytes as estimates to confirm after the first real build (`pio run -t size`
or the map file).
