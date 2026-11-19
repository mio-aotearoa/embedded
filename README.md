# meshcore-tdeck-and-photoframe

Two ESP32-S3 firmwares, built almost entirely out of pinned upstream projects:

| Firmware | Hardware | What runs | Upstream code |
|---|---|---|---|
| [`firmware/tdeck-plus`](firmware/tdeck-plus) | LilyGO T-Deck Plus | wadamesh (MeshCore mesh stack + LVGL touch UI) with a Kiwix ZIM reader from SD | `jade-on-mesh/wadamesh-tak` (which pins its MeshCore fork), `facebook/zstd` |
| [`firmware/reterminal-e1002`](firmware/reterminal-e1002) | Seeed reTerminal E1002 | esp32-photoframe + AirDrop receive (ESP32Drop) | `aitjcize/esp32-photoframe`, `s-iwaki-d/ESP32Drop` |

Both devices are ESP32-S3 boards with PSRAM, which is what the upstream
projects need.

## Rule for upstream code

Nothing from an upstream project is copied into this repository.

* Upstream projects are **git submodules** under `third_party/`, pinned to a
  commit (`git submodule status`).
* Changes that an upstream project needs are kept as a **patch series** in
  `patches/<project>/`, applied with `scripts/patches.sh`.
* `scripts/patches.sh rebase` moves a submodule to a new upstream commit and
  re-applies the series with three-way merge, so keeping up with upstream is a
  normal git operation rather than a re-port.

```bash
git clone --recurse-submodules <this-repo> && cd <this-repo>
./scripts/patches.sh apply          # apply local series to third_party/*
./scripts/patches.sh status         # what is applied, what would conflict
./scripts/patches.sh rebase wadamesh origin/main   # forward-port
```

Two series exist: esp32-photoframe (a build hook and a boot-mode hook) and
wadamesh (the app integrations: Kiwix, ESP-AI and the periodic table — build
wiring, drawer tiles and a Reader bridge). ESP32Drop and zstd are used exactly
as published, from the submodule working tree.

## The two designs

**T-Deck Plus** — wadamesh is already a complete firmware: MeshCore radio,
routing, contacts and channels, an LVGL touch UI with an app drawer, a Files
browser and a Reader that renders local HTML from the card. None of that is
reimplemented here. The Kiwix reader (`firmware/tdeck-plus/kiwix/`) is a new
app page compiled into wadamesh's own T-Deck PlatformIO environment by a single
patch: it lists `.zim` archives on the card, searches their title index, writes
the chosen article's HTML to the card and hands it to the firmware's Reader, so
articles get the native scrolling, keyboard/trackball navigation and back bar.
ZIM parsing is the only format code we own (libzim needs a desktop C++17/ICU/XZ
runtime and has no ESP32 target).

Both apps can fill the card themselves: Kiwix downloads a `.zim` from a URL over
Wi-Fi (a shared `common/http_download.cpp` task writes to `<name>.part` and
renames only after the last byte), and the `ESP-AI` page fetches the
slvDev/esp32-ai `model.bin` and `tokenizer.json` into
`/esp32ai/tinystories/` when they are missing, checking the pinned byte sizes.
The LLM itself is not generated on the device yet: upstream's runtime reads its
weights from a flash mapping, and the 14.9 MB model does not fit this board's
flash layout, so the page manages the assets and says that generation needs the
runtime port.

The `Elements` page is the periodic table from
[El-EnderJ/NeoCalculator](https://github.com/El-EnderJ/NeoCalculator)
(GPL-3.0-or-later). Their LVGL 9 app is not ported; instead
`third_party/neocalculator` is a pinned submodule and the page includes its
`ChemDatabase.h`, so the element data and 18×10 layout stay upstream while only
the grid drawing is new code. Tap a cell for mass, category, electronegativity
and electron configuration.

The `Download` page fetches any `http://` or `https://` URL straight to the card
(`/sd/downloads/`, or `/sd/ime/` for the pinyin word-list presets), using the
same downloader task as Kiwix/ESP-AI, with progress and cancel.

The `LocalSend` page sends a file from the card to a LocalSend peer on the LAN.
No upstream client code is used (the official app is Flutter and the other
implementations are Rust/Go/Win32), so this is our own protocol v2 client: peer
discovery, `prepare-upload`, a streamed `upload`, and `cancel` on failure. It
sends over plain HTTP and does not receive.

The browser is **in the image**. It reuses TactileBrowser's
`tactilebrowser_core` (Lexbor) from a pinned submodule and adds only the platform
layer (`firmware/tdeck-plus/browser/`): an LVGL 8.3 renderer and a fetcher.
There is no second web page — the drawer has one **Web** tile, wadamesh's own
Reader page, and it lays fetched markup out with this renderer (the core's CSS
subset, inline scripts on Duktape 2.7.0) while keeping its text view as the
fallback for pages the layout engine cannot handle.
Lexbor's legacy encoding tables are `const` through
`patches/TactileBrowser/`, so ~760 KB of tables live in flash instead of DRAM:
the image uses **7,135,237 of 8,126,464 bytes of flash (87.8%)** and **149,640 of
327,680 bytes of RAM (45.7%)**. See
[`docs/architecture.md`](docs/architecture.md) for the measured before/after.

Browser-engine options (TactileBrowser, eBrowser, Agave) are compared in
[`docs/web-browser-options.md`](docs/web-browser-options.md), which also covers
what the renderer supports and what it drops, and
`firmware/tdeck-plus/partitions_tdeck_browser.csv` — a 7.75 MB single-app
partition table with no OTA slot (updates over USB), keeping the map tiles and
user data. librime is not included (Boost/leveldb/marisa/OpenCC, no ESP32 port).
`third_party/lvgl_pinyin_ime` is pinned instead: its LVGL 9.5 widget does not
build against this firmware's LVGL 8.3, but its vendored libgooglepinyin engine
is portable and `scripts/pinyin-dict.sh` builds the 1 MB dictionary for
`/sd/ime/dict_pinyin.dat`. The remaining work — an LVGL 8 candidate UI and CJK
glyphs — is tracked in [`docs/cjk-rendering.md`](docs/cjk-rendering.md).

**reTerminal E1002** — esp32-photoframe owns storage, image processing and
sleep. The AirDrop bridge supplies two builds: `switch` chooses exclusive
Wi-Fi or AirDrop at boot; `coex` attempts a bounded AirDrop window alongside
a connected channel-6 Wi-Fi station and HTTP server. ESP32Drop is built behind
a small Arduino shim with local radio-ownership and listener-shutdown patches.
The concurrent build needs hardware validation.

See [`docs/architecture.md`](docs/architecture.md) for the details, the
constraints behind each choice, and the known gaps.

## Building the T-Deck Plus firmware

The base is **jade-on-mesh/wadamesh-tak** (`third_party/wadamesh`, pinned by
`.gitmodules`). `firmware/tdeck-plus/build.sh` — which `nix run .#firmware`
runs — refuses to build if that submodule is not the wadamesh checkout, applies
`patches/wadamesh`, then runs
`pio run -e LilyGo_TDeck_companion_radio_touch`.

Prerequisites: the submodules, and network access on the first build, because
PlatformIO downloads the espressif32 platform, the Xtensa toolchain and
wadamesh's `lib_deps` into `.platformio/`. Supported Nix systems:
`x86_64-linux`, `aarch64-linux`, `aarch64-darwin`, and `x86_64-darwin`.

```bash
git submodule update --init --recursive

# PlatformIO and the toolchain from the Nix store; extra args go to pio run
nix run .#firmware
nix run .#firmware -- -t upload

# or with the toolchain already on PATH
nix develop -c firmware/tdeck-plus/build.sh
firmware/tdeck-plus/build.sh -t upload
PIOENV=<wadamesh env> firmware/tdeck-plus/build.sh
```

The images are written to `third_party/out/` (wadamesh's `merge-bin.py` copies
them there from `third_party/wadamesh/out/`), named after the PlatformIO env,
with a timestamped copy beside them:

```
third_party/out/LilyGo_TDeck_companion_radio_touch.bin
third_party/out/LilyGo_TDeck_companion_radio_touch-<stamp>-<sha>.bin
```

`nix build` itself is hermetic (no network in the Nix sandbox), so it builds
what can be built offline — the ZIM parser's host test:

```bash
nix build            # result/bin/test_zim_archive, prints PASS
nix build .#host-tests
nix build .#tdeck-builder   # the firmware builder script (run: result/bin/tdeck-build)
```

PlatformIO's own downloads cannot happen inside a sandboxed build, and this
flake does not vendor a 1 GB toolchain closure. The builder therefore runs as
an ordinary program (`nix run`) with a project-local `.platformio/` cache.

## Status

The T-Deck firmware is wadamesh plus the Kiwix app and the ESP-AI model manager: no
MeshCore adapter or UI fork remains in this repository, only the parser, the
picker, the downloader and the model page.

The **T-Deck Plus image compiles**: `nix run .#firmware` with the pinned
toolchain (PlatformIO/espressif32 6.11.0) produces
`third_party/out/LilyGo_TDeck_companion_radio_touch.bin` (3,859,664 bytes),
with `KiwixApp::launch`, `Esp32AiApp::launch`, `ZimArchive::searchTitles` and
`httpDownloadStart` linked in. It has **not been flashed or run on hardware**.
**Both E1002 variants compile and link** with ESP-IDF 6.0.3 / GCC 15.3.0.
Merged first-flash images are in `build/e1002-{switch,coex}/firmware.bin`.
AirDrop/Wi-Fi operation has not been verified on hardware.

## Licensing

* wadamesh and its MeshCore fork — GPL-3.0
* esp32-photoframe — GPL-3.0
* ESP32Drop — 0BSD
* zstd — BSD-3-Clause / GPL-2.0 dual

The combined firmwares are therefore GPL-3.0. ESP32Drop's 0BSD terms are
compatible. `third_party/*` keeps each project's own LICENSE file; this
repository's own code is GPL-3.0 (see `LICENSE`).

## Building the reTerminal E1002 firmware

With ESP-IDF 6.0 activated (install separately; works on aarch64-darwin with
Espressif’s macOS toolchain). `nix develop` provides Node.js, CMake, Ninja and
esptool for asset generation and flashing:

```bash
nix develop -c bash -lc '. "$IDF_PATH/export.sh" && firmware/reterminal-e1002/build.sh'
# or, with IDF already on PATH:
firmware/reterminal-e1002/build.sh   # builds switch and coex
```

See [build and flash instructions](firmware/reterminal-e1002/README.md) for
prerequisites, mode selection, checksums and hardware limits.

Host unit tests for both firmwares (ZIM parser, AirDrop companion link) need no
ESP32 toolchain:

```bash
nix build .#host-tests \
  && ./result/bin/test_zim_archive \
  && ./result/bin/test_airdrop_companion
```
