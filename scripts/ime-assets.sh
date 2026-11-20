#!/usr/bin/env bash
# Build every IME card asset under out/ime/:
#   dict_pinyin.dat  — libgooglepinyin system dictionary
#   skk_mdic.bin     — Japanese kanji (SKK middle)
#   cjk14.bin        — LVGL 8 bitmap font (ASCII + kana + pinyin charset)
#
# Usage: scripts/ime-assets.sh
# Publish: gh release create ime-v1 out/ime/*  (then Download presets resolve)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/out/ime"
mkdir -p "$out"

"$root/scripts/pinyin-dict.sh" "$out/dict_pinyin.dat"
"$root/scripts/skk-dict.sh" "$out/skk_mdic.bin"
"$root/scripts/cjk-font.sh" "$out/cjk14.bin"

echo "==> ready in $out"
ls -la "$out"
