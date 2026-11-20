# T-Deck Plus

wadamesh-tak as published, plus apps in this directory compiled into its T-Deck
PlatformIO env by `patches/wadamesh/`.

## Apps

| Drawer | Code | Notes |
|---|---|---|
| Kiwix | `kiwix/` | ZIM on SD → Reader; catalogue + download over Wi‑Fi |
| ESP-AI | `esp32ai/` | TinyStories from SD (`third_party/esp32-ai`) |
| Browser | `browser/` | Lexbor (TactileBrowser core) + Duktape inline JS |
| Web | upstream | wadamesh Reader (text extract) |
| LocalSend | `localsend/` | protocol v2 send + receive; ArduinoJson + HTTP |
| Download | `downloader/` | any HTTP(S) URL → SD |
| Elements | `periodic/` | NeoCalculator `ChemDatabase.h` |
| OutRun | `arcade3d/` | host + TFT shim; engine submodule |
| Voxel Flight | `voxel_flight/` | host; engine submodule |

Shared under `common/`: `http_fetch`, `http_download`, `sd_atomic`,
`task_spawn`, `psram_alloc`, `game_steer`, `game_canvas`, `url_util`,
`app_toast`, `cpu_boost`. IME under `ime/`: Japanese (romaji→kana→SKK) and
Chinese (libgooglepinyin); dictionaries + `cjk14.bin` on `/ime/` (see
`docs/cjk-rendering.md`, `scripts/ime-assets.sh`).

## Layout

```
firmware/tdeck-plus/   apps + build.sh
patches/wadamesh/      0001 apps wiring · 0002 Wi‑Fi off on battery (default on)
patches/esp32s3-arcade-3d/   manual steer / hill throttle
patches/voxel-flight/        RGB565 host framebuffer
third_party/wadamesh|esp32s3-arcade-3d|voxel-flight|…
```

Engines are not vendored: pin + patch only.

## Build

```bash
git submodule update --init --recursive
firmware/tdeck-plus/build.sh            # LilyGo_TDeck_companion_radio_touch
firmware/tdeck-plus/build.sh -t upload
nix run .#firmware
```

`build.sh` applies the wadamesh (and game) series, refuses a non-wadamesh-tak
checkout, then `pio run`. Status: `./scripts/patches.sh status wadamesh`.

## Kiwix

1. Put `.zim` files on the SD card (or download from the catalogue).
2. **Apps → Kiwix** → pick archive → title search → article opens in Reader.

Limits: title-index search only; no ZIM-internal link follow; LZMA2/XZ clusters
rejected (use zstd); large articles capped. See `docs/architecture.md`.
