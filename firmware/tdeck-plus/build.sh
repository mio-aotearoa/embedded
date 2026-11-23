#!/usr/bin/env bash
# Build the T-Deck Plus firmware: wadamesh (pinned submodule) + this repo's
# Kiwix app.
#
# The base is jade-on-mesh/wadamesh-tak, pinned by .gitmodules. The Kiwix reader is
# new code in this repository, compiled into wadamesh's own T-Deck PlatformIO
# environment through patches/wadamesh/. Nothing is copied into the submodule.
#
# Usage: firmware/tdeck-plus/build.sh [extra pio args, e.g. -t upload]
# Nix:   nix run .#firmware        (same build, with PlatformIO from the store)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENV_NAME="${PIOENV:-LilyGo_TDeck_companion_radio_touch}"

die() { echo "error: $*" >&2; exit 1; }

cd "$ROOT"

[ -f third_party/wadamesh/platformio.ini ] || die "third_party/wadamesh is missing; run: git submodule update --init --recursive"
remote="$(git -C third_party/wadamesh remote get-url origin 2>/dev/null || true)"
case "$remote" in
    *jade-on-mesh/wadamesh-tak*) ;;
    *) die "third_party/wadamesh must be jade-on-mesh/wadamesh-tak (found: ${remote:-no remote}); run: git submodule sync third_party/wadamesh" ;;
esac

# wadamesh carries the app hooks; TactileBrowser makes Lexbor's encoding
# tables const so the browser's HTML/CSS engine fits in flash, not DRAM;
# esp32-ai adds the LLM_DEQ_ROW hook the ESP-AI app's SD-backed weights need.
./scripts/patches.sh apply wadamesh TactileBrowser TactileBrowserCore \
  esp32-ai esp32s3-arcade-3d voxel-flight

# Duktape ships raw sources only: build its amalgamation (build/duktape/).
./scripts/duktape.sh

# One core dir for both build paths (script and `nix run .#firmware`), so the
# espressif32 platform and lib_deps are downloaded only once. Force an absolute
# path: a broken nix mkShell once exported the literal string '$PWD/.platformio'.
case "${PLATFORMIO_CORE_DIR:-}" in
  ""|'$PWD/.platformio'|'$PWD/'*) export PLATFORMIO_CORE_DIR="$ROOT/.platformio" ;;
  /*) ;;
  *) export PLATFORMIO_CORE_DIR="$ROOT/.platformio" ;;
esac
mkdir -p "$PLATFORMIO_CORE_DIR"

echo "==> wadamesh base: $(git -C third_party/wadamesh rev-parse --short HEAD)"
echo "==> PlatformIO core dir: $PLATFORMIO_CORE_DIR"
echo "==> building $ENV_NAME in third_party/wadamesh"

cd "$ROOT/third_party/wadamesh"
exec pio run -e "$ENV_NAME" "$@"
