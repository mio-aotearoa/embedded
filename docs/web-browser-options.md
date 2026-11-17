# Web browser options for the T-Deck Plus

What is actually available for this board, what each candidate would cost to
bring in, and which one is worth attempting. This is a survey plus a
recommendation, not an integration: nothing here is compiled into the firmware
yet.

## What the firmware has today

wadamesh already ships a text browser: its Reader does an HTTP(S) GET, follows
redirects, strips scripts/styles, decodes entities, keeps block structure, and
renders the result with the firmware's own scrolling, keyboard/trackball
navigation and back bar (`openReaderPage()` in `UITask.cpp`). Kiwix articles are
handed to that same page. It has no CSS layout, images, forms or JavaScript, and
its HTTPS path uses `setInsecure()` with no certificate store.

**Can the Reader do CSS or JS? No, by construction.** `ReaderContent::htmlToText`
(`src/ui-touch/ReaderContent.cpp`) runs one tag-stripping pass: it skips the body
of every `<script>` and `<style>` element, drops all other tags, decodes
entities, and keeps block/line boundaries plus `<a href>` targets. Nothing
computes a style, lays out a box, or executes code — so a page arrives as plain
text with link targets, regardless of what its CSS says. Media and forms are
dropped for the same reason.

That is the baseline any browser engine has to beat, and it costs 0 bytes of
new flash.

## Constraints that decide the answer

* **Build system:** wadamesh is an Arduino/PlatformIO app. A candidate has to
  become a PlatformIO library/component or be compiled through
  `build_src_filter`, not just a CMake project.
* **LVGL version:** wadamesh pins **LVGL 8.3.11**. eBrowser and Agave's
  `watchweb` are written against LVGL 9.x, so either the engine or the firmware
  has to move.
* **Flash:** the app partition is 3.875 MB (0x3E0000) and the current image,
  with Kiwix, ESP-AI, the downloader and the periodic table, is **3,868,217 B**
  — about **195 KB of headroom**. The two 3.875 MB slots exist so OTA can keep
  working; the table below is how that changes.
* **RAM:** 512 KB internal SRAM + 8 MB PSRAM. The S3 has no MMU, and QuickJS or
  a Lexbor DOM is a heap consumer; PSRAM latency is also where LVGL already
  spends its budget.
* **Display:** 320×240, 16-bit, SPI. Full-page layout has to be tiled to keep
  scrolling smooth.

## The candidates

| Project | Target / portability | Rendering | JavaScript | Dependencies | Licence |
|---|---|---|---|---|---|
| **TactileBrowser** ([NellowTCS](https://github.com/NellowTCS/TactileBrowser)) | ESP32 and ESP32-S3 targets; also SDL2 desktop and WASM. Tightly coupled to the Tactility app framework. | Lexbor HTML, own CSS subset (tag/class/id, box model, gradients, text align), LVGL rendering | Duktape on ESP32 (Elk on desktop) | Lexbor, LVGL, Tactility build | check repo `LICENSE` |
| **eBrowser** ([embeddedos-org](https://github.com/embeddedos-org/eBrowser)) | C11, modular engine with a platform abstraction; in-tree ports are EoS, SDL2 and WASM. The project's site claims an ESP32-S3/8 MB PSRAM target, but there is no ESP32 port in the repository. | Own HTML5/CSS/DOM/layout engine, LVGL UI, software raster | Optional, **off by default** | LVGL, mbedTLS (fetched by CMake) | MIT |
| **Agave** ([onecoolx](https://github.com/onecoolx/agave)) | C++11, full engine with a C API; platforms listed are Linux/Windows plus LVGL/Qt/SDL/custom framebuffer. No ESP32 or ESP-IDF port or example. | Own HTML5/CSS3 layout engine (flexbox, grid, animations), Picasso 2D software raster, RGB565 output | QuickJS ES2020 | libcurl, FreeType2, SQLite3, LVGL for the example | BSD-3 (QuickJS MIT, FreeType FTL/GPL) |
| **babe32** ([alunmorris](https://github.com/alunmorris/babe32-ESP32-web-browser-for-cheap-black-display)) | ESP32-S3 + 480×320 touch board (Arduino/PlatformIO); not a T-Deck target, but the closest board class | Text HTML subset, headings/links/bold, optional images, forms, LVGL | none | LVGL, TLS, optional proxy | check repo `LICENSE` |

The table in the request matches this: TactileBrowser and eBrowser are ESP32
projects, Agave has "advanced" CSS and QuickJS but its firmware story is an
example, not a port. "Claimed support" for eBrowser is exactly that — a claim on
the project site, with no ESP32 code to build from in the repository.

## Making room: partition layouts

The app slot is 3.875 MB only because the table keeps a second slot for OTA.
`firmware/tdeck-plus/partitions_tdeck_browser.csv` is a drop-in table for a
browser build, same 16 MB flash, same tiles/spiffs offsets:

| Layout | App slot | OTA | Map tiles | User data |
|---|---|---|---|---|
| today (`partitions_tdeck_touch.csv`) | 3.875 MB ×2 | yes | 4.75 MB | kept |
| **single app** (`partitions_tdeck_browser.csv`) | **7.75 MB** (0x7C0000) | no (USB only) | 4.75 MB | kept |
| single app, no tiles | 12.5 MB (0xC80000) | no | lost | kept |
| two 5 MB slots | 5 MB ×2 | yes | ~2.5 MB | moved (wiped) |

The first option is the one worth taking: it doubles the app budget without
touching `tiles` (0x7D0000) or `spiffs` (0xC90000), so a USB re-flash keeps the
tile cache and every stored preference/contact/channel. The cost is OTA: the
`otadata` partition is gone and `app0` is a `factory` app, so updates are
USB-only (`CAP_OTA` is 0 in this build, which compiles the on-device update UI
out). Removing otadata does not grow the app by itself — app partitions start on
a 64 KB boundary, and 0x10000 is the first after the partition table — so its
8 KB went to NVS (20 KB → 28 KB). Dropping the tile cache buys another 4.75 MB
but loses offline maps, and moving SPIFFS to fit two larger slots wipes user
data on the first flash.

Changing the table is not the same as fitting a browser: 7.75 MB of flash still
leaves the S3's 512 KB SRAM and PSRAM bandwidth as the binding constraint for a
DOM/CSS/JS engine. It removes the "there is no room at all" objection, which is
why the engine work below is worth doing against this table rather than the
current one.

## Why none of them drops in

* **No room in the image.** A DOM + CSS layout engine plus a JS engine is
  hundreds of KB of code and heap; the app slot has ~199 KB. Bringing any of the
  three in means a new partition table (drop the second OTA slot and/or the map
  tile cache) or building a *separate* browser firmware instead of an app in
  wadamesh. That is a variant decision, not an app-page addition.
* **LVGL 9 vs 8.3.11.** eBrowser and Agave's example expect LVGL 9. wadamesh's
  UI is built against 8.3.11; upgrading the whole firmware to 9 is a separate,
  large change. TactileBrowser is the only candidate already rendering through
  LVGL on an ESP32-S3, but it assumes the Tactility framework's build and APIs.
* **Build system.** Only babe32 is an Arduino/PlatformIO project like wadamesh.
  eBrowser/Agave/TactileBrowser are CMake/IDF trees; each would need a
  component wrapper and a platform port (display flush, input, TLS, timers).
* **TLS and proxy reality.** wadamesh has no certificate store. Direct HTTPS to
  most sites works only `setInsecure()`; sites that block embedded clients need
  a proxy (babe32 ships one for exactly this reason). A browser that cannot
  verify certificates is a privacy claim, not a security one.

## Recommendation

1. **Ship the Reader as "the browser"** and improve it incrementally (images,
   simple forms, a readable-mode extraction). It is already integrated, fits,
   and is the only option with zero porting cost.
2. **If a real engine is required, try TactileBrowser first.** It is the only
   candidate that targets ESP32-S3 and renders through LVGL, and Duktape is the
   smaller JS engine. Expect to either adopt Tactility's framework or extract
   its Lexbor/CSS/layout layer into a wadamesh app page — and expect LVGL 9.
3. **eBrowser is the second choice** if the Tactility coupling is unacceptable:
   C11, MIT, modular, JS off by default, mbedTLS already in the tree. The work
   is an ESP-IDF/PlatformIO component port plus its LVGL 9 dependency; the
   claimed ESP32-S3 target is unverified.
4. **Agave is the most capable and the least practical here.** Full CSS3 and
   QuickJS need libcurl, FreeType and SQLite, and it has no MCU port; porting it
   is a project of its own, not an app.
5. If the goal is "read web pages on the T-Deck" rather than "run a browser
   engine", babe32's approach (text subset, forms, optional proxy-side images)
   is the closest fit to the remaining flash and is a better template for
   extending the Reader than any full engine.

## Verification status

Read from each project's README, site and source layout on 2026-11-17. No
candidate was built or linked into the firmware, so none of their flash/RAM
figures are measured here; the ~199 KB headroom is from this repository's own
build (3,859,664 B image against a 3,875 MB app partition). Before committing to
one, build it for `esp32s3` first and measure the image and steady-state heap.
