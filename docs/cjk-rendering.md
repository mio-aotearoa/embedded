# CJK on the T-Deck

Physical-keyboard IME with dictionaries and a bitmap font on the SD card
(`/ime/`). No librime; no flash-embedded CJK font (factory slot is tight).

## Assets (download to `/ime/`)

| File | Role | Build |
|---|---|---|
| `dict_pinyin.dat` | libgooglepinyin system dict (~1.0 MB) | `scripts/pinyin-dict.sh` |
| `skk_mdic.bin` | Japanese kanji (SKK middle, ~210 KB) | `scripts/skk-dict.sh` |
| `cjk14.bin` | LVGL 8 bin font, 14px/1bpp (~0.5 MB), including Japanese punctuation | `scripts/cjk-font.sh` |

One-shot: `scripts/ime-assets.sh` → `out/ime/`. Publish the three files on the
`ime-assets` orphan branch (Download presets fetch
`raw.githubusercontent.com/mio-aotearoa/embedded/ime-assets/…`), and optionally
also as a GitHub Release:

```bash
gh release create ime-v1 out/ime/dict_pinyin.dat out/ime/skk_mdic.bin out/ime/cjk14.bin \
  --title "IME card assets" --notes "CJK IME dictionaries + font for /ime/"
```

Or copy the three files onto the card under `/ime/` over USB.

## Usage

Long-press a text field (Kiwix / Browser / Download) to cycle **Off → Japanese →
Chinese → Off**.

* **Japanese:** type romaji, Space → kana; with `skk_mdic.bin`, Space then offers
  kanji (1–9 / Enter / Space to cycle; Esc dismisses).
* **Chinese:** type pinyin, Space → candidates (same keys). Needs `dict_pinyin.dat`.
* Glyphs need `cjk14.bin` (otherwise tofu boxes).

Engine: LVGL-free `ime_engine` + libgooglepinyin from
`third_party/lvgl_pinyin_ime` (not the LVGL 9 on-screen keyboard widget).
