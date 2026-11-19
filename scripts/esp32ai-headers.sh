#!/usr/bin/env bash
# Regenerate ESP-AI decode/encode headers from the pinned TinyStories tokenizer.
#
# Inputs (either path works):
#   $1  tokenizer.json   (default: downloads the Hugging Face release pin)
#
# Outputs (committed under firmware/tdeck-plus/esp32ai/generated/):
#   vocab.h                 token id -> UTF-8 bytes (decode)
#   tokenizer_encoder.h     BTK1 asset for runtime/bpe_tokenizer.h (encode)
#
# Requires: uv (or PYTHON with tokenizers==0.23.1), curl, shasum/sha256sum.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/firmware/tdeck-plus/esp32ai/generated"
ESP="$ROOT/third_party/esp32-ai"
PIN_SHA="4e28163669f2249af31a528a54fc25064dcbd0a34edbfa7bedb16d2d600ec7ae"
PIN_BYTES=1788896
URL="https://huggingface.co/slvDev/esp32-ai-tinystories/resolve/main/tokenizer.json"

mkdir -p "$OUT" "$ROOT/.cache/esp32ai"
TOK="${1:-$ROOT/.cache/esp32ai/tokenizer.json}"

if [ ! -f "$TOK" ]; then
    echo "==> downloading tokenizer.json"
    curl -fsSL -o "$TOK.part" "$URL"
    mv "$TOK.part" "$TOK"
fi

actual_bytes="$(wc -c < "$TOK" | tr -d ' ')"
if [ "$actual_bytes" != "$PIN_BYTES" ]; then
    echo "error: tokenizer.json is $actual_bytes bytes, expected $PIN_BYTES" >&2
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    echo "$PIN_SHA  $TOK" | sha256sum -c --status -
else
    actual="$(shasum -a 256 "$TOK" | awk '{print $1}')"
    [ "$actual" = "$PIN_SHA" ] || {
        echo "error: tokenizer.json SHA-256 mismatch" >&2
        exit 1
    }
fi

[ -d "$ESP" ] || {
    echo "error: third_party/esp32-ai missing; init the submodule" >&2
    exit 1
}

echo "==> vocab.h (decode table)"
uv run --no-project --with 'tokenizers==0.23.1' python \
    "$ESP/firmware/esp32_tinystories/tools/generate_vocab.py" \
    --tokenizer "$TOK" --out "$OUT/vocab.h"

# BTK1 generator refuses added_tokens; TinyStories only adds <|endoftext|> which
# ASCII encode never emits. Strip them for the encoder asset only.
echo "==> tokenizer_encoder.h (BTK1)"
ENC_JSON="$ROOT/.cache/esp32ai/tokenizer.encoder.json"
python3 - "$TOK" "$ENC_JSON" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    cfg = json.load(f)
cfg["added_tokens"] = []
with open(dst, "w") as f:
    json.dump(cfg, f)
PY
uv run --no-project --with 'tokenizers==0.23.1' python \
    "$ESP/firmware/esp32_barista/tools/generate_tokenizer_header.py" \
    --tokenizer "$ENC_JSON" --out "$OUT/tokenizer_encoder.h"

echo "wrote:"
ls -la "$OUT/vocab.h" "$OUT/tokenizer_encoder.h"
