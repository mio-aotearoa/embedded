#!/usr/bin/env bash
# Build E1002 firmwares: switch, coex (IDF), and/or open (FoA).
# Usage: ./firmware/reterminal-e1002/build.sh [all|switch|coex|open|airdrop] …
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$ROOT/firmware/reterminal-e1002/build.py" "$@"
