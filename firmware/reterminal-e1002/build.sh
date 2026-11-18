#!/usr/bin/env bash
# Build both E1002 variants, or pass switch / coex to select one.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$ROOT/firmware/reterminal-e1002/build.py" "$@"
