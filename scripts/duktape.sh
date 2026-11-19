#!/usr/bin/env bash
# Fetch Duktape's release amalgamation into build/duktape/.
#
# Duktape is an embedded ES5.1 engine (~350 KB of flash here). Its git repository
# ships only the raw sources: the single-file amalgamation is produced by a
# Python 2-era configure script that no longer runs on Python 3, and its Makefile
# needs `bc`. The official release tarball already contains the generated
# duktape.c, duktape.h and duk_config.h, so this pins that tarball by SHA-256 and
# extracts just those three files.
#
# Nothing is copied into the repository: this is a build input, like the
# PlatformIO packages, and it is cached under build/ (git-ignored).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/duktape"
VERSION="2.7.0"
URL="https://duktape.org/duktape-${VERSION}.tar.xz"
SHA256="90f8d2fa8b5567c6899830ddef2c03f3c27960b11aca222fa17aa7ac613c2890"
CACHE="$ROOT/build/duktape-${VERSION}.tar.xz"

if [ -f "$OUT/duktape.c" ] && [ -f "$OUT/duk_config.h" ]; then
    echo "==> duktape ${VERSION} amalgamation present"
    exit 0
fi

mkdir -p "$ROOT/build"
if [ ! -f "$CACHE" ]; then
    echo "==> downloading duktape ${VERSION}"
    curl -fsSL -o "$CACHE.part" "$URL"
    mv "$CACHE.part" "$CACHE"
fi

# Prefer sha256sum (Linux / newer macOS); fall back to shasum -a 256 (BSD).
if command -v sha256sum >/dev/null 2>&1; then
    echo "$SHA256  $CACHE" | sha256sum -c --status - || {
        echo "error: $CACHE does not match the pinned SHA-256" >&2
        rm -f "$CACHE"
        exit 1
    }
elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$CACHE" | awk '{print $1}')"
    if [ "$actual" != "$SHA256" ]; then
        echo "error: $CACHE does not match the pinned SHA-256" >&2
        rm -f "$CACHE"
        exit 1
    fi
else
    echo "error: need sha256sum or shasum to verify duktape tarball" >&2
    exit 1
fi

mkdir -p "$OUT"
tar -xJf "$CACHE" -C "$OUT" --strip-components=2 \
    "duktape-${VERSION}/src/duktape.c" \
    "duktape-${VERSION}/src/duktape.h" \
    "duktape-${VERSION}/src/duk_config.h"
echo "==> duktape ${VERSION} extracted to ${OUT#$ROOT/}"
