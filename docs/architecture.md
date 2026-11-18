# Architecture

## Repository layout

```
third_party/
  wadamesh/              jade-on-mesh/wadamesh-tak (submodule, pinned; T-Deck firmware)
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

### Browser — TactileBrowser's core, linked and in the image

**Status: the whole engine fits.**

`third_party/TactileBrowser` is pinned and its core is reused as-is: it parses
HTML and CSS with Lexbor and renders through a `RenderInterface` of function
pointers (`create_label`, `create_button`, `set_bg_gradient`,
`register_link_handler`, …). That interface is why it drops in beside LVGL 8.3
without the LVGL 9 problem the survey found: the core contains **no `lv_*` calls
at all**. `firmware/tdeck-plus/browser/` is the platform code — every renderer
callback wrapping LVGL widgets, an HTTP(S) fetcher on its own task (redirects
followed, TLS `setInsecure()` like the Reader, 16 KB page cap, freed after
rendering), and the page chrome: URL bar, Go, keyboard, scrolling white view,
status line, links that navigate.

**What the link needed.** Lexbor's legacy multibyte tables
(`lxb_encoding_multi_*_map`: JIS0208, JIS0212, Big5, EUC-KR, GB18030) are ~760 KB
of *initialized writable* data, which ESP-IDF places in `.dram0.data` — on the S3
that overflowed DRAM by **613,584 B**. `patches/TactileBrowser/0001` makes every
table in that generated file `const` (the series applies to the nested Lexbor
submodule via `patches/TactileBrowser/dir`), which moves them to
`.flash.rodata`. `.dram0.data` is now 37,672 B and the T-Deck image uses
**149,640 of 327,680 bytes of RAM (45.7%)** and **7,135,237 of 8,126,464 bytes of
flash (87.8%)** — about 0.95 MB of flash left for the remaining apps. The pinned
tree's own render tests still pass with the const tables.

Lexbor also gets a PSRAM allocator through `lexbor_memory_setup()` in
`browser_app.cpp`: its default `malloc` serves internal RAM, where a real page's
DOM (several times its markup) does not fit, so every page used to fall back to
the Reader's text pass. It is installed before the first Lexbor allocation,
falls back to internal RAM only if PSRAM is exhausted, and the fonts and the
Reader's text buffer are untouched.

**JavaScript.** Inline scripts run on **Duktape 2.7.0 (ES5.1)**, the engine
TactileBrowser's own ESP32 build uses. `scripts/duktape.sh` fetches the pinned
release tarball by SHA-256, because Duktape's repository ships only raw sources
(its amalgamation generator is Python 2-era). `js_engine.cpp` gives a page
`document.write()`, `document.title` and `console.log()`: written markup replaces
the `<script>` block before layout, and the last log line or script error goes to
the status bar. `firmware/tdeck-plus/test/host/test_browser_js.cpp` covers
generated markup, console output, skipped external scripts, plain pages and error
reporting.

**Limits, stated plainly.** There is no DOM and no events: `document.write()` and
`console.log()` are the whole JS surface, so frameworks and `onclick` handlers do
nothing. External `<script src>` and external stylesheets are not fetched and
images are not decoded. CJK glyphs still need the font work in
[`cjk-rendering.md`](cjk-rendering.md).

### On-device downloader

`firmware/tdeck-plus/downloader/` is a page, not a second transport: it fills in
the URL and destination for the same `common/http_download.cpp` worker Kiwix and
ESP-AI use (HTTPClient GET, redirects followed, TLS `setInsecure()`, one
transfer at a time, `<dest>.part` renamed only after the last byte). It accepts
`http://` and `https://` URLs, derives a safe filename from the URL, writes to
`/sd/downloads/` (presets write to `/sd/ime/`), shows percentage when the server
sends a length, can cancel, and cleans up on page teardown.

Presets point at the pinyin engine's source word lists on GitHub, pinned to the
submodule commit. Those are the inputs to `scripts/pinyin-dict.sh`; the binary
dictionary itself is still built on a host, so downloading the lists is a
convenience (a device with only a card can hold the data), not a way to produce
`dict_pinyin.dat` on the S3 yet.

### LocalSend — a send-only client written here, no code copied

The official LocalSend app is Flutter; the other implementations are Rust, Go
and Win32, and none targets ESP-IDF, so there is nothing to vendor and nothing
to patch. `firmware/tdeck-plus/localsend/` is this repository's own code written
from the protocol description (`localsend/protocol`, v2): it discovers peers
(multicast announcement listener on 224.0.0.167:53317, then a /24 sweep of
`GET /api/localsend/v2/info`), calls `prepare-upload`, then streams one SD file
to `upload` and calls `cancel` on failure. JSON for those three small documents
is built and read by hand, so no JSON library is added.

What it deliberately does not do: receive files, speak HTTPS (a peer announcing
`"protocol":"https"` is not listed rather than failing later), folders or
multi-file batches. Transfer progress comes from an `lv_timer` polling the
worker task, like the Kiwix/ESP-AI downloader, so the UI never blocks.

### Periodic table (`El-EnderJ/NeoCalculator`) — data from the pinned submodule

NeoCalculator's periodic table is an LVGL 9 app (`lv_obj_delete`,
`lv_event_get_layer`, custom draw), and this firmware is pinned to 8.3.11, so
the app is not lifted and, per this repository's rule, none of its source is
copied. `third_party/neocalculator` is a pinned submodule and
`periodic_app.cpp` includes its `src/apps/ChemDatabase.h` directly (it only
needs `<cstdint>`, so it compiles unchanged): the 118 elements, the category
enum and the 18×10 `TABLE_LAYOUT` all stay upstream, under their own
GPL-3.0-or-later headers.

`periodic_app.cpp` is the only new code: it draws the grid in a single LVGL
object instead of 118 buttons, `LV_EVENT_DRAW_MAIN` paints each cell (category colour, selected cell
outlined) and `LV_EVENT_PRESSED` converts the pointer position to a row/column
and looks the element up in `kLayout`. The strip under the grid shows
`#Z symbol name — mass` and `category · EN · configuration`. It uses the same
`AppPage` chrome as Kiwix/ESP-AI, so the tall "< Periodic Table" bar, back
affordance and async teardown are the firmware's, not a copy.

What is not carried over: the Molar mass and Equation-balance tabs, search, and
the trackball `NAV_LUT` (the grid is touch-first; the trackball moves the
pointer and taps a cell).

### wadamesh changes (patch series)

`patches/wadamesh/0001-kiwix-esp32ai-periodic-apps.patch` is the whole
integration:

1. `#include "kiwix_app.h"` / `"esp32ai_app.h"` / `"periodic_app.h"` beside the
   other app modules.
2. `APPACT_KIWIX`, `APPACT_ESP32AI` and `APPACT_PERIODIC` in the drawer action
   enum, a `Kiwix` tile (gated on `CAP_FILESYSTEM`, since the app needs the
   card), an `ESP-AI` tile and an `Elements` tile, and their `appLaunch()` cases.
3. `extern "C" void wadameshOpenReaderUrl(const char*)` at the end of
   `UITask.cpp`, a one-line wrapper around the existing `openReaderPage()`.
4. `platformio.ini` (T-Deck env only): include paths for
   `firmware/tdeck-plus/{kiwix,common,esp32ai,periodic}` and
   `third_party/zstd/lib`, the
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

* Images and math: the renderer decodes neither. Articles up to 128 KB get CSS
  and box layout; larger ones fall back to the Reader's text pass, which drops
  CSS, tables and images.
* Full-text search: only the ZIM title index is searched (prefix match).
* Following ZIM-internal links from an article; the picker is the navigation.
* Multiple archives open at once, and archives above the PSRAM/parse limits.
* `LZMA2`/`XZ` clusters; those archives need re-exporting with zstd.

### Web browser options

The T-Deck's Web page is wadamesh's Reader with our renderer merged in: the
pinned TactileBrowser core (Lexbor) does the layout and the Reader's text pass is
the fallback. The engines that could have replaced it — eBrowser, Agave — and
what each would cost in flash, build integration and LVGL version are compared
in [`web-browser-options.md`](web-browser-options.md).

## Firmware 2 — reTerminal E1002

The [E1002 build instructions](../firmware/reterminal-e1002/README.md) describe
both variants and their limits. `components/airdrop_bridge` connects
ESP32Drop's receiver to photoframe's existing display pipeline. The Arduino
shim is in `components/esp32drop_compat`.

`patches/ESP32Drop/` adds APSTA radio ownership, listener shutdown and Mbed TLS 4
compatibility. `patches/esp32-photoframe/` adds variant hooks, HTTP mutation
and sleep/rotation guards, and configuration-aware partition generation.

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
`jade-on-mesh/wadamesh-tak` before patching, and print the pinned base revision.

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

The T-Deck Plus image is compiled and linked with the pinned toolchain
(PlatformIO/espressif32 6.11.0, `LilyGo_TDeck_companion_radio_touch`). Both E1002 variants compile and link with ESP-IDF 6.0.3 / GCC 15.3.0.
Nothing has been flashed or run on hardware. What was actually checked:

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

* The T-Deck env compiles: `nix run .#firmware` finished in 92 s with
  `http_download.cpp`, `esp32ai_app.cpp`, `kiwix_app.cpp` and `zim_reader.cpp`
  in the build, no compiler warnings from them, and 7,135,237 B of image in the 7.75 MB
  `factory` slot (no OTA: USB updates only), with `PeriodicApp::launch`,
  `LocalSendApp::launch` and the LocalSend task symbols present (the
  browser renderer is linked in too). `nm` finds `KiwixApp::launch()`,
  `Esp32AiApp::launch()`, `ZimArchive::searchTitles()` and
  `httpDownloadStart()` in the ELF.

Not checked: hardware behaviour — Wi-Fi download throughput, TLS handshakes, SD write failures at
the end of a large file, the Reader's rendering of a downloaded article, and
esp32-ai token generation.

## Known gaps / next steps

* AirDrop accepts JPEG/PNG; other image formats are rejected.
* Kiwix: article images/CSS and full-text search are out of scope; LZMA2
  archives need a zstd re-export.
* `patches.sh rebase` uses `git apply --3way`; conflicts are resolved in the
  submodule and captured with `patches.sh refresh <project>`.
