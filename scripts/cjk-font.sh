#!/usr/bin/env bash
# Build a single LVGL 8 binary CJK/kana font for /ime/cjk14.bin.
#
# Uses Noto Sans SC + lv_font_conv. Ranges: ASCII, kana, Japanese punctuation,
# half/fullwidth forms, and the pinyin engine's valid_utf16.txt codepoints. The
# full CJK Unified Ideographs block is too large for the card-asset budget.
#
# Usage: scripts/cjk-font.sh [output-path]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/out/ime/cjk14.bin}"
ime="$root/third_party/lvgl_pinyin_ime"
hzs="$ime/data/dict/valid_utf16.txt"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

command -v npx >/dev/null || { echo "error: npx (Node) required for lv_font_conv" >&2; exit 1; }
[ -f "$hzs" ] || { echo "error: missing $hzs (checkout lvgl_pinyin_ime)" >&2; exit 1; }

# Subset OTF (~16 MB) — enough glyphs for the pinyin charset + kana.
font_url="https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
font_file="$tmpdir/NotoSansSC-Regular.otf"

# Prefer a local cache under out/ so repeat runs are fast.
cache="$root/out/cache/NotoSansSC-Regular.otf"
if [ -f "$cache" ] && [ "$(wc -c < "$cache" | tr -d ' ')" -gt 1000000 ]; then
    cp "$cache" "$font_file"
else
    echo "==> fetching Noto Sans SC subset (large; once)"
    mkdir -p "$(dirname "$cache")"
    rm -f "$cache"
    if ! curl -fsSL -o "$cache" "$font_url"; then
        echo "error: could not download Noto Sans SC" >&2
        exit 1
    fi
    cp "$cache" "$font_file"
fi

# Build a symbols file: ASCII + kana + every BMP codepoint listed in valid_utf16.
syms="$tmpdir/symbols.txt"
python3 - "$hzs" "$syms" <<'PY'
import sys
hzs, out = sys.argv[1], sys.argv[2]
chars = set()
for cp in range(0x20, 0x7F):
    chars.add(chr(cp))
for cp in range(0x3040, 0x30A0):  # hiragana
    chars.add(chr(cp))
for cp in range(0x30A0, 0x3100):  # katakana
    chars.add(chr(cp))
for cp in range(0x3000, 0x3040):  # Japanese punctuation and CJK symbols
    chars.add(chr(cp))
for cp in range(0xFF00, 0xFFF0):  # half/fullwidth forms used by SKK candidates
    chars.add(chr(cp))
# valid_utf16.txt is UTF-16LE code units, one per line or a blob — try both.
data = open(hzs, "rb").read()
# File is a sequence of UTF-16LE code units (no BOM required).
if len(data) >= 2:
    text = data.decode("utf-16-le", errors="ignore")
    for ch in text:
        o = ord(ch)
        if 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF:
            chars.add(ch)
# Cap to keep the bin near ~1.5 MB at 14px/1bpp: prefer frequency order if huge.
# valid_utf16 is already the pinyin charset (~few thousand).
with open(out, "w", encoding="utf-8") as f:
    f.write("".join(sorted(chars, key=ord)))
print(f"symbols: {len(chars)} codepoints", file=sys.stderr)
PY

mkdir -p "$(dirname "$out")"
echo "==> lv_font_conv → $out"
npx --yes lv_font_conv@1.5.2 \
    --font "$font_file" \
    --size 14 \
    --bpp 1 \
    --format bin \
    --no-compress \
    --no-prefilter \
    --symbols "$(cat "$syms")" \
    --output "$out"

size="$(wc -c < "$out" | tr -d ' ')"
printf 'font: %s (%s bytes)\n' "$out" "$size"
if [ "$size" -gt 2000000 ]; then
    echo "warning: font > 2 MB; consider a smaller subset" >&2
fi
printf 'copy it to the card as /ime/cjk14.bin\n'
