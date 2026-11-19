# CJK on the T-Deck (not shipped)

320×240, LVGL 8.3, ~7.75 MB factory slot. Plan if/when needed:

* Precompiled LVGL bitmap font (frequency-trimmed coverage), not FreeType.
* Prefer glyphs in flash (XIP) or a swappable file on SD.
* Input: small pinyin table IME (e.g. libgooglepinyin dict on `/sd/ime/`), not
  librime (Boost/leveldb — no ESP32 port).

`third_party/lvgl_pinyin_ime` is pinned for the dict path;
`scripts/pinyin-dict.sh` builds `dict_pinyin.dat`. No CJK font or IME UI is in
the image yet.
