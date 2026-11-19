#!/usr/bin/env bash
# Build the libgooglepinyin binary dictionary (dict_pinyin.dat).
#
# The engine and the source word lists both live in the pinned
# third_party/lvgl_pinyin_ime submodule, so nothing is vendored here. The output
# is a card asset, not source: copy it to /sd/ime/dict_pinyin.dat.
#
# Usage: scripts/pinyin-dict.sh [output-path]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ime="$root/third_party/lvgl_pinyin_ime"
eng="$ime/src/third_party/libgooglepinyin"
raw="$ime/data/dict/rawdict_utf16_65105_freq.txt"
hzs="$ime/data/dict/valid_utf16.txt"
out="${1:-$root/out/dict_pinyin.dat}"

for f in "$raw" "$hzs" "$eng/tools/pinyinime_dictbuilder.cpp"; do
    [ -f "$f" ] || { echo "missing $f (is the submodule checked out?)" >&2; exit 1; }
done

mkdir -p "$(dirname "$out")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Host-only tool: the engine is plain C++ with a C API, so the host compiler
# builds it unchanged (no ESP-IDF headers involved).
g++ -std=c++17 -O2 -I"$eng/include" -I"$eng/port" -o "$tmp/dictbuilder" \
    "$eng/tools/pinyinime_dictbuilder.cpp" "$eng"/src/*.cpp

"$tmp/dictbuilder" "$raw" "$hzs" "$out" >/dev/null

if size="$(stat -c%s "$out" 2>/dev/null)"; then
    :
elif size="$(stat -f%z "$out" 2>/dev/null)"; then
    :
else
    size="$(wc -c < "$out" | tr -d ' ')"
fi
printf 'dictionary: %s (%s bytes)\n' "$out" "$size"
printf 'copy it to the card as /sd/ime/dict_pinyin.dat\n'
