#!/usr/bin/env bash
# Fetch the SKK middle dictionary binary used by Japanese kanji lookup.
#
# Layout matches Tamakichi/arduino_skk (ssk_mdic.bin): 12-byte header + index +
# keyword/candidate data. We keep the on-card name skk_mdic.bin.
#
# Usage: scripts/skk-dict.sh [output-path]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/out/ime/skk_mdic.bin}"
url="https://raw.githubusercontent.com/Tamakichi/arduino_skk/main/ssk_mdic.bin"

mkdir -p "$(dirname "$out")"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

echo "==> fetching $url"
curl -fsSL -o "$tmp" "$url"

size="$(wc -c < "$tmp" | tr -d ' ')"
if [ "$size" -lt 10000 ]; then
    echo "error: download too small ($size bytes)" >&2
    exit 1
fi

mv "$tmp" "$out"
trap - EXIT
printf 'dictionary: %s (%s bytes)\n' "$out" "$size"
printf 'copy it to the card as /ime/skk_mdic.bin\n'
