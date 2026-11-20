# Architecture

## Layout

```
third_party/          pinned submodules (wadamesh-tak, zstd, photoframe, Drop,
                      FoA, TactileBrowser, esp32-ai, arcade-3d, voxel-flight, …)
components/           ESP-IDF bits for E1002 (esp32drop_compat, airdrop_bridge)
patches/<project>/    quilt-style series; dirty submodule = series applied
firmware/tdeck-plus/  T-Deck apps + build.sh
firmware/reterminal-e1002{,-open}/
scripts/patches.sh
```

## T-Deck Plus

Base firmware is wadamesh-tak. This repo adds drawer apps and shared helpers;
integration is `patches/wadamesh/` only (no fork of UI/radio logic).

| App | Role |
|---|---|
| Kiwix | ZIM v5/v6 on `/sd` → HTML to Reader; catalogue + `http_download` |
| ESP-AI | SD TinyStories via `third_party/esp32-ai` + `LLM_DEQ_ROW` |
| Browser | TactileBrowser core (Lexbor) + LVGL 8.3 glue + Duktape inline JS |
| Web | Upstream Reader (`htmlToText`) |
| LocalSend | Protocol v2 on `:53317`; ArduinoJson + WiFi/HTTPClient (no ESP32 LS lib) |
| Download | Same downloader worker, arbitrary URL |
| Elements | NeoCalculator `ChemDatabase.h` grid |
| OutRun / Voxel | Submodule engines + AppPage hosts; 1:1 RGB565, no stretch |

`common/http_fetch.cpp`: shared HTTP(S) GET with manual redirects + TLS buffers
in PSRAM (used by downloads, Kiwix catalogue, Browser).
`common/http_download.cpp`: one transfer to SD (`.part` then rename) on top of
`http_fetch`, polled from LVGL timers.
`common/app_toast.h` → `wadameshShowAlert` (sys layer, visible over AppPages).

Wi‑Fi: Settings → Battery → “Wi‑Fi off on battery” (default on) holds
`wifiConfigWantsWifi()` while unplugged without rewriting the radio toggle.

### Deliberate limits

* Kiwix: title-index search; no ZIM-internal links; no article images/CSS in
  Reader; LZMA2/XZ rejected.
* Browser: no DOM/events; no external scripts/CSS/images; CJK glyphs need font
  work ([`cjk-rendering.md`](cjk-rendering.md)).
* Mesh image codec needs ~GB RAM — store/forward only
  ([`mesh-image-raw-plan.md`](mesh-image-raw-plan.md)).

Partition: single ~7.75 MB factory app (USB update, no OTA twin) so Lexbor
fits; tiles/spiffs kept.

## reTerminal E1002

* **switch** — Wi‑Fi XOR AirDrop (ESP32Drop exclusive).
* **coex** — ch6 APSTA dual-protocol RX experiment.
* **open** — FoA `foa_sta` + `foa_awdl` on ESP32-S3 (patched FoA / esp-wifi-hal).

Bridge: `components/airdrop_bridge` → photoframe display. See
[`firmware/reterminal-e1002/README.md`](../firmware/reterminal-e1002/README.md).

## Build

* `nix build` / `.#host-tests` — hermetic ZIM (and companion) host tests.
* `nix run .#firmware` — apply patches, `pio run` (needs network for PlatformIO
  cache in `.platformio/`).
* E1002: IDF export + `firmware/reterminal-e1002/build.sh`.
