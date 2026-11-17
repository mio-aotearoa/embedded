# Architecture

## Repository layout

```
third_party/
  wadamesh/              ALLFATHER-BV/wadamesh      (submodule, pinned; T-Deck firmware)
  zstd/                  facebook/zstd              (submodule, pinned; ZIM clusters)
  esp32-photoframe/      aitjcize/esp32-photoframe  (submodule, pinned)
  ESP32Drop/             s-iwaki-d/ESP32Drop        (submodule, pinned)
components/               ESP-IDF components consumed by the photoframe build
  esp32drop_compat/       minimal Arduino.h for ESP32Drop's ports
  airdrop_bridge/         AirDrop receive boot window, writes a file for photoframe
patches/
  <project>/series        ordered list of patch files
  <project>/NNNN-*.patch  quilt-style patches
scripts/patches.sh        apply | revert | status | refresh | rebase
firmware/
  tdeck-plus/             Kiwix app compiled into wadamesh's PlatformIO env
  reterminal-e1002/       build wrapper over the patched photoframe project
```

`third_party` is never edited by hand outside the patch workflow; a dirty
submodule working tree is exactly "the patch series is applied".

## Firmware 1 — T-Deck Plus: wadamesh + Kiwix

### Why wadamesh is the base

The earlier plan for this board was MeshCore plus Meshtastic's `device-ui`
wrapped in an `IClientBase` adapter. That meant owning an adapter, a second
build system and a UI that does not know MeshCore. wadamesh is already the
firmware that plan was trying to assemble: it is a MeshCore fork (pulled as
`lib_deps`, so its mesh stack stays upstream) with an LVGL touch UI built for
the T-Deck: app drawer, chats, contacts, map, Files browser, a text Reader, and
board support for the panel, GT911 touch, trackball and keyboard. It builds
with its own `platformio.ini`, so there is no adapter to port.

The only functionality this repository adds to the T-Deck is Kiwix. It is new
code because no upstream project provides a ZIM reader that fits.

### Composition

```
app drawer "Kiwix"  (one tile, added by patches/wadamesh)
        |
        v
KiwixApp  - list *.zim on the card (/sd)
          - search the ZIM title index (keyboard)
          - write the article as HTML to /meshcomod/kiwix/article.htm
        |
        v
wadamesh Reader (openReaderPage, existing screen)
        |  local HTML: wrapping, scrolling, keyboard/trackball, back bar
        v
SD card
```

`firmware/tdeck-plus/kiwix/zim_reader.cpp` parses ZIM v5/v6: header, MIME list,
path/title pointer lists, content and redirect dirents, uncompressed and
Zstandard clusters (decompressed with the pinned `third_party/zstd`, built
decompress-only). LZMA2/XZ clusters are rejected with a message instead of
being misread.

`firmware/tdeck-plus/kiwix/kiwix_app.cpp` is the picker. It uses wadamesh's
`AppPage` helpers, which every full-screen tool page in the firmware uses
(Spectrum, Discover, Snake, Reader), so the status bar, the "< Kiwix" back
affordance, async teardown and input routing behave like the built-in apps.

Articles are rendered by the firmware's own Reader: it already opens local HTML
from the card (`sd:/...`), and its `ReaderContent` pass strips scripts/styles,
decodes entities and keeps block structure. That is why
`firmware/tdeck-plus` no longer carries its own HTML-to-text reducer. ZIM
internal links are rewritten from `<a>` to `<span>` before the hand-off,
because the Reader would resolve their relative hrefs against the card and
dead-end.

SD is mounted by wadamesh itself at `/sd` on the shared radio/display SPI bus;
the Kiwix app only uses the `SD` object, never `SD.begin()`, so it cannot fight
the firmware's mount policy or open-file budget.

### Why not an existing ZIM reader

* **libzim** (openzim) is the reference implementation, but it is a desktop
  C++17 library built with meson and depending on ICU, XZ and zstd. Its
  allocator, file and thread assumptions do not fit a 320x240 ESP32-S3
  firmware, and porting it would mean maintaining a fork far larger than the
  parser here. The format documentation plus the small parser is the smaller
  change.
* **babe32-ESP32-web-browser-for-cheap-black-display** is an HTML/CSS browser
  for 1-bit e-paper. It has no ZIM support and its renderer is tailored to a
  different display class, so it would replace wadamesh's theme, fonts and input
  handling for no gain. The "offline Wikipedia on ESP32" articles describe the
  same idea (ZIM on SD) rather than a reusable reader component.
* wadamesh's Reader is the reuse that fits: same panel, same fonts, same
  keyboard/trackball policy, already shipping.

### wadamesh changes (patch series)

`patches/wadamesh/0001-kiwix-and-esp32ai-apps.patch` is the whole integration:

1. `#include "kiwix_app.h"` / `"esp32ai_app.h"` beside the other app modules.
2. `APPACT_KIWIX` and `APPACT_ESP32AI` in the drawer action enum, a `Kiwix`
   tile (gated on `CAP_FILESYSTEM`, since the app needs the card) plus an
   `ESP-AI` tile, and their `appLaunch()` cases.
3. `extern "C" void wadameshOpenReaderUrl(const char*)` at the end of
   `UITask.cpp`, a one-line wrapper around the existing `openReaderPage()`.
4. `platformio.ini` (T-Deck env only): include paths for
   `firmware/tdeck-plus/{kiwix,common,esp32ai}` and `third_party/zstd/lib`, the
   zstd decompress-only defines, and `build_src_filter` entries compiling our
   apps, the shared downloader and zstd's `common/` + `decompress/` sources into
   the firmware. The filter paths climb out of the project
   (`../../../firmware/...`) exactly as wadamesh's own variant filter climbs to
   `../variants/`.

No wadamesh logic is changed; `scripts/patches.sh revert wadamesh` restores the
pinned tree.

### Wi-Fi downloads

`firmware/tdeck-plus/common/http_download.cpp` is the one transfer path both
apps use: an `HTTPClient` GET (redirects followed, `identity` encoding) written
straight to `<dest>.part` on the SD card from a 12 KB FreeRTOS task, renamed to
its real name only after the last byte arrived. Callers poll
`httpDownloadState()` from an `lv_timer`, so the LVGL task never blocks on the
network or the card.

Two properties are deliberate:

* **TLS is `setInsecure()`.** The firmware has no certificate store, exactly
  like the Reader's HTTPS path, so a download proves nothing about the server.
  The esp32-ai assets pin their byte sizes (from upstream's `fetch_model.sh`) so
  a truncated transfer is refused; ZIM downloads have no pinned size or hash.
* **One transfer at a time,** with cancellation on page teardown. A cancelled
  or failed transfer deletes the `.part` file and leaves the previous file of
  that name untouched.

### Deliberately out of scope (today)

* Images, CSS, tables, math: the Reader is a text view.
* Full-text search: only the ZIM title index is searched (prefix match).
* Following ZIM-internal links from an article; the picker is the navigation.
* Multiple archives open at once, and archives above the PSRAM/parse limits.
* `LZMA2`/`XZ` clusters; those archives need re-exporting with zstd.

## Firmware 2 — reTerminal E1002: photoframe + AirDrop

### The radio is the constraint

ESP32Drop's AWDL implementation holds 2.4 GHz channel 6 permanently to stay in
step with the AWDL availability window. It cannot coexist with
`WiFi.begin()`, and esp32-photoframe is a Wi-Fi device (HTTP API, URL rotation,
SNTP). The only correct composition is time-multiplexing at boot:

```
power on + trigger button held
        |
        v
  airdrop_bridge: esp_netif + AWDL + TLS listener (no Wi-Fi station)
        |  receive one AirDrop transfer -> /storage/images/airdrop-<id>.<ext>
        v
  photoframe image pipeline (image_processor/display_flow)
        |
        v
  deep sleep
```

Without the trigger the firmware boots exactly as upstream photoframe.

### Why a boot mode and not a background service

`ad_begin()` binds a TLS listener to the link-local address the AWDL netif
owns and takes a 32 KB contiguous internal-RAM task stack. Starting it beside
photoframe's Wi-Fi stack would (a) fail the radio claim and (b) risk the
internal-RAM budget mbedTLS needs. A boot window that ends in a display refresh
and deep sleep is the composition that keeps both projects inside their stated
constraints.

### ESP32Drop in an ESP-IDF build

ESP32Drop is an Arduino library, but its actual dependencies are narrow: the
two port files include `<Arduino.h>` for `millis()` (plus comments about
`Serial`, which the library deliberately does not own) and otherwise use
ESP-IDF APIs directly. `components/esp32drop_compat/include/Arduino.h` provides
`millis()`, `micros()`, `delay()`, `delayMicroseconds()` and a staged `Serial`
sink. No upstream source is changed.

The receive build is only two translation units:

* `ESP32Drop/src/awdl/port/awdl_port_esp32.cpp`
* `ESP32Drop/src/airdrop/port/ad_port_esp32.cpp`

(the core codecs are header-only, and the sender/BLE files are not compiled).

### photoframe changes (patch series)

`patches/esp32-photoframe/`:

1. `0001-build-allow-external-components.patch` — lets the upstream CMake project
   pick up this repository's `components/` directory. One line.
2. `0002-airdrop-boot-window.patch` — adds the component to `main`'s
   `PRIV_REQUIRES` and a short boot hook in `app_main` that runs the AirDrop
   window and hands the file to `display_flow_stream_file()`.

Everything else (storage, image processing, palette, deep sleep, HTTP API) is
upstream, untouched.

## Build with Nix

`flake.nix` exposes three things:

* `nix build` (default) / `nix build .#host-tests` — a **hermetic** build of the
  ZIM parser host test. It compiles `firmware/tdeck-plus/kiwix/zim_reader.cpp`
  against the pinned `zstd` flake input and prints `PASS`. This works inside
  the Nix sandbox because it needs no network.
* `nix run .#firmware` — the real firmware build. It applies
  `patches/wadamesh`, then runs `pio run -e LilyGo_TDeck_companion_radio_touch`
  in `third_party/wadamesh` with `PLATFORMIO_CORE_DIR=$PWD/.platformio`.
* `nix develop` — the same PlatformIO/git/python environment as `nix run`, plus
  `firmware/tdeck-plus/build.sh`.

Why the firmware is not a plain `nix build` derivation: PlatformIO downloads the
`espressif32` platform, the Xtensa toolchain and every `lib_deps` entry at build
time. A sandboxed Nix build has no network and this flake does not vendor a
~1 GB toolchain closure, so the download has to happen in a normal process
(`nix run`) or a shell (`nix develop`). The builder is still just the script in
`firmware/tdeck-plus/build.sh` with pinned inputs from `flake.lock`; only the
PlatformIO cache lives outside the store.

`build.sh` (and the flake builder) assert that `third_party/wadamesh` really is
`ALLFATHER-BV/wadamesh` before patching, and print the pinned base revision.

## Requested additions and why they are not wired in

These were evaluated against the pinned upstreams; none is a small app-page
change, so none is faked in the tree.

### esp32-ai (`slvDev/esp32-ai`) — model assets on the card, runtime not ported

It is not a library or an AI service client: it is a set of standalone Arduino
sketches running a 28.9M-parameter TinyStories model locally, with 14.9 MB of
4-bit weights in a **dedicated `model` flash partition** (`partitions.csv`:
app 1 MB, model 14.9 MB), plus a custom runtime and Xtensa SIMD code. The
T-Deck's 16 MB flash is already fully allocated (two 3.875 MB OTA slots +
4.75 MB map tiles + 3.375 MB SPIFFS), and wadamesh is a ~2.55 MB app, so the
model cannot get a flash partition here without dropping OTA and the tile
cache.

What this repository does wire in is the storage half: `firmware/tdeck-plus/esp32ai/`
is an `ESP-AI` app page that keeps the release's `model.bin` and
`tokenizer.json` at `/esp32ai/tinystories/` on the SD card, shows whether each
file is present and complete, and fetches it over Wi-Fi through the shared
downloader when it is not. The byte sizes are pinned from upstream's
`scripts/fetch_model.sh`, so a truncated transfer is refused.

Text generation still needs a real port: upstream's loader memory-maps the
weights from a flash partition and compiles model-specific headers at deploy
time, so pointing it at an SD file means replacing its weight access path with
per-token `pread`s and reproducing the header generation here. That is a port
of the runtime, not an app tile, and it is not faked with a placeholder
generator. The page says so on screen.

### CJK rendering

The screen and flash budget for Chinese/Japanese/Korean text, the font format
choice (precompiled LVGL bitmap font, not FreeType) and the pinyin table IME
that would be needed to type it are analysed in
[`cjk-rendering.md`](cjk-rendering.md). It is a build-time asset decision plus
an input app, not a one-line change, so no font is checked in here.

### Rime (`rime.im`) / librime

librime is a desktop C++17 IME engine built on Boost, glog, leveldb,
marisa-trie, yaml-cpp and OpenCC; it has no ESP32 port and no plan for one. The
S3 has 512 KB SRAM and no MMU for that stack. A CJK input method on this board
has to be a small table engine (pinyin syllable table + dictionary on SD + an
LVGL candidate bar) that feeds the existing text fields — a different project
from embedding librime.

### MeshCore image messages (`zjs81/meshcore-open`)

The transport (GRP_DATA chunks, command 62 / response 27 / data type 0xAE1C,
XOR parity, metadata) is small and could be received on the T-Deck. The
**codec is not**: upstream requires an ONNX runtime, a ~1 GB model bundle and
reconstruction peaks above 2 GB of RAM. The ESP32-S3 has 512 KB SRAM and 8 MB
PSRAM, so it cannot encode or reconstruct these images at all — this is a
hardware limit, not a missing port. Firmware could reassemble chunks and save
the `.mimg` stream to SD for a phone/desktop to decode later, but there is no
"run it on the T-Deck" option. (wadamesh also speaks the MeshCore companion
protocol, so the companion app can already carry the packets; the UI just has
no image bubble.)

### lwext4 and LittleFS

* LittleFS is **already supported on both targets**: wadamesh mounts a LittleFS
  `tiles` partition and uses LittleFS/SPIFFS for user state; esp32-photoframe's
  `storage.c` mounts `esp_littlefs` and falls back to formatting it.
* lwext4 has no ESP-IDF/ESP32 glue at all (the repo contains no esp32 example or
  porting layer). It needs a block-device backend plus a VFS layer. On the
  T-Deck the card is mounted by wadamesh as FAT at `/sd`; ext4 would be a
  mutually exclusive mount mode for the same slot, not an additional filesystem
  next to FAT. On the E1002 the storage path is LittleFS on flash; an ext4 SD
  volume would need the same block-device work. That is a feature branch of its
  own, and it cannot be compile-checked in this checkout.

## UI performance

The UI performance question was checked against
[`andrisgauracs/bevel-os`](https://github.com/andrisgauracs/bevel-os), which is
an ESP32-S3 desktop written from scratch (no LVGL) and reports 60–76 FPS. Its
main techniques are: render into internal RAM and write to PSRAM once, track
damage rectangles instead of redrawing frames, keep the scan-out interrupt and
the GUI task on the same core, and keep the pointer out of the framebuffer.

The first two are already in wadamesh, with measurements recorded in the code:

* `LV_DISP_DEF_REFR_PERIOD` is **16 ms** in `include/lv_conf.h`, not LVGL's
  30 ms default, so the refresh loop is not capped at ~33 FPS.
* The T-Deck draw band is a 12800 px (40-row) PSRAM band chosen by the frame
  benchmark on hardware (comment at the draw-buffer allocation in `UITask.cpp`),
  and LVGL only renders invalidated areas.
* LVGL is built `-O2` with `lv_draw_sw_blend.c` / `lv_draw_label.c` in IRAM
  (`custom_lvgl_o2` / `custom_lvgl_iram` in `platformio.ini`), and the UI task
  is what drives `lv_timer_handler`.

The remaining candidates are the ones bevel-os cannot hand over directly:
band size and buffer placement (PSRAM vs internal DMA RAM) are already tuned by
that benchmark, and the per-frame cost of `lv_layer_top` overlays is a
structural issue in wadamesh's app pages, noted at the drawer in `UITask.cpp`.
Changing either without running the on-device frame benchmark would be a guess,
so this repository does not claim a speed-up: measure with the existing
`BENCH_BAND_W` / `BENCH_BUF_PSRAM` benchmark build before changing a default.

## Verification status

There is no PlatformIO or ESP-IDF toolchain in this checkout, so neither image
has been compiled or flashed. What was actually checked:

* `scripts/patches.sh apply|status|revert` round-trips both series against the
  pinned commits and leaves the submodules clean.
* Every ESP32Drop and photoframe symbol the bridge and the boot hook call was
  matched to the pinned public headers (names and signatures).
* The ZIM parser is portable C++ and is built and run on the host against the
  pinned zstd sources: `test_zim_archive.cpp` writes a minimal valid ZIM v6
  (header, MIME list, dirents, pointer lists, uncompressed cluster) and reads
  back titles, blobs and the main page.
* The wadamesh patch's anchors (`APPACT_` enum, drawer tile table,
  `appLaunch()`, `openReaderPage`) were taken from the pinned commit; the
  filter paths and include paths were checked against the submodule's location.
* The Arduino shim (`components/esp32drop_compat`) is syntax-checked with host
  g++ against stub ESP-IDF/FreeRTOS headers.
* Shell scripts are `bash -n` clean.

Not checked: PlatformIO compilation of the wadamesh + Kiwix env (LVGL /
Arduino headers are not installed here), ESP-IDF compilation of the AirDrop
listener, and any hardware behaviour.

## Known gaps / next steps

* First build on a machine with the toolchains; expect API drift fixes in
  `kiwix_app.cpp` (LVGL 8 list/textarea calls, matching wadamesh's pinned
  LVGL) and in the bridge.
* AirDrop file types other than JPEG/PNG are written to storage and reported in
  the log, not displayed.
* Kiwix: article images/CSS and full-text search are out of scope; LZMA2
  archives need a zstd re-export.
* `patches.sh rebase` uses `git apply --3way`; conflicts are resolved in the
  submodule and captured with `patches.sh refresh <project>`.
