# T-Deck Plus — wadamesh + Kiwix

The T-Deck Plus runs **wadamesh** as published: its MeshCore radio stack, LVGL
touch UI, contacts/channels, Files, Reader and app drawer. This repository adds
two apps to it: **Kiwix**, which browses ZIM archives (offline Wikipedia and
friends) from the SD card, and **ESP-AI**, which manages the slvDev/esp32-ai
model assets on the card. Both can fetch their data over Wi-Fi.

Upstream is not forked or copied. `third_party/wadamesh` is a pinned submodule;
the apps live here and are compiled into wadamesh's own T-Deck PlatformIO
environment by a single patch (`patches/wadamesh/0001-kiwix-esp32ai-periodic-apps.patch`).

## Layout

```
firmware/tdeck-plus/
  build.sh              apply the patch, then pio run in third_party/wadamesh
  kiwix/
    zim_reader.{h,cpp}  ZIM v5/v6 parser (uncompressed + zstd clusters)
    kiwix_app.{h,cpp}   app page: archive list -> title search -> Reader
  esp32ai/
    esp32ai_app.{h,cpp}  download UI + chat once the model is on the card
    llm_port.{h,cpp}     SD-backed slvDev/esp32-ai TinyStories runtime
    generated/           vocab.h + tokenizer_encoder.h (scripts/esp32ai-headers.sh)
  common/
    http_download.{h,cpp}  one HTTP(S) -> SD transfer, on its own task
  arcade3d/             OutRun AppPage host + TFT_eSPI canvas shim (engine is a submodule)
  voxel_flight/         Voxel Flight AppPage host (engine is a submodule)
  test/host/            host test for the parser (no ESP32 needed)
patches/wadamesh/
  0001-kiwix-esp32ai-periodic-apps.patch  build wiring + drawer tiles + Reader bridge
patches/esp32s3-arcade-3d/
  0001-manual-steer-no-lateral-autopilot.patch
patches/voxel-flight/
  0001-wadamesh-host-framebuffer.patch
third_party/esp32s3-arcade-3d/   davidmonterocrespo24/esp32s3-arcade-3d (submodule)
third_party/voxel-flight/        lewtheegg/voxel-flight (submodule)
```

## How the pieces fit

```
app drawer "Kiwix"
        |
        v
KiwixApp (our app page, wadamesh AppPage chrome)
        |  list *.zim on /sd, search titles via ZIM title index
        v
zim_reader: read cluster -> article HTML
        |  write /meshcomod/kiwix/article.htm
        v
wadamesh Reader (openReaderPage)  <- existing firmware screen
        |  local HTML: scroll, keyboard/trackball, back bar
        v
SD card
```

The Reader (**Web** tile) is wadamesh's upstream text browser: HTTP(S) or local
HTML stripped to readable text with link targets. **Apps → Browser** is the
separate Lexbor HTML/CSS app (`firmware/tdeck-plus/browser/`). Kiwix still opens
articles in the Reader. ZIM-internal links are turned into plain text because the
Reader would resolve them against the card and miss; following links inside an
archive is not implemented.

## Build

The base is `jade-on-mesh/wadamesh-tak`, pinned by `.gitmodules`; `build.sh` refuses
to run if `third_party/wadamesh` is not that checkout and prints the base
revision it is patching.

```bash
git submodule update --init --recursive
firmware/tdeck-plus/build.sh            # pio run -e LilyGo_TDeck_companion_radio_touch
firmware/tdeck-plus/build.sh -t upload

# with Nix (PlatformIO, git, python from the store; downloads cached in .platformio/)
nix run .#firmware
nix develop -c firmware/tdeck-plus/build.sh
```

`nix build` is the hermetic path and builds only what needs no network: the ZIM
parser host test (`nix build`, output `result/bin/test_zim_archive`). The
firmware build needs PlatformIO's downloads, which a sandboxed Nix build cannot
do, hence `nix run .#firmware` / `nix develop`.

Requirements: PlatformIO with the ESP32 platform (wadamesh pins
`platformio/espressif32@6.11.0`), and wadamesh's own `lib_deps`
(its MeshCore fork `meshcomod#core-v1.18.0`, LVGL, RadioLib, ...). The patch is
applied automatically; `./scripts/patches.sh status wadamesh` shows it.

## Using it

1. Copy one or more `.zim` files to the root of the SD card.
2. Open **Apps → Kiwix** and pick an archive.
3. Type a title prefix and press Enter (the T-Deck keyboard) to search, or open
   the archive's main page.
4. Tap a result. The article opens in the Reader; the back bar returns to the
   firmware.

## Limitations

* Text/HTML articles only. Kiwix opens them in the Reader's text pass (no CSS or
  tables). Use **Apps → Browser** for Lexbor layout of fetched pages. The first
  ZIM image is still shown separately before the article. Full-text (Xapian)
  search is not used — search is the ZIM title index, so it matches title
  prefixes only.
* LZMA2/XZ clusters (pre-libzim-8 archives) are rejected with a message; those
  archives need re-exporting with zstd.
* Articles larger than 220 KB are refused rather than truncated.
* One archive is open at a time; the working file is
  `/meshcomod/kiwix/article.htm` on the card.
* The image is flashed over USB (`firmware/tdeck-plus/build.sh -t upload`,
  or `nix run .#firmware -- -t upload`); it links at 7,135,237 B (87.8 % of
  the app slot). See `docs/architecture.md` for the measured figures.
