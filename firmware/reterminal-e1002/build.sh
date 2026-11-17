#!/usr/bin/env bash
# Build the AirDrop-enabled esp32-photoframe firmware for the Seeed reTerminal
# E1002. Requires ESP-IDF (idf.py on PATH), Node.js/npm for photoframe's web UI
# assets, and the submodules + patch series.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PHOTOFRAME="$ROOT/third_party/esp32-photoframe"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/reterminal-e1002}"
BOARD="seeedstudio_reterminal_e1002"

command -v idf.py >/dev/null || {
    echo "error: idf.py not found. Source ESP-IDF's export.sh first." >&2
    exit 1
}
command -v npm >/dev/null || {
    echo "error: npm not found. photoframe embeds a built web UI (step 1)." >&2
    exit 1
}

"$ROOT/scripts/patches.sh" apply esp32-photoframe

# Upstream's own build.py produces the embedded web UI and splash assets; the
# CMake project expects them to exist. Run only those two steps, then drive the
# firmware build ourselves so the extra sdkconfig fragment is included.
echo "==> upstream pre-build steps (web UI + splash)"
(cd "$PHOTOFRAME" && python3 build.py --board "$BOARD" --step webapp --step splash)

echo "==> firmware build"
VERSION="airdrop-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo local)"

exec idf.py \
    -C "$PHOTOFRAME" \
    -B "$BUILD_DIR" \
    -D "IDF_TARGET=esp32s3" \
    -D "SDKCONFIG_DEFAULTS=$PHOTOFRAME/sdkconfig.defaults;$PHOTOFRAME/boards/sdkconfig.defaults.$BOARD;$ROOT/firmware/reterminal-e1002/sdkconfig.airdrop" \
    -D "FIRMWARE_VERSION=$VERSION" \
    build
