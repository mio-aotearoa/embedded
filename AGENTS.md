# Agent notes

## Prefer existing / public code over homemade

When adding or changing firmware apps:

1. **Reuse first.** Prefer a pinned `third_party/` submodule, an already-linked
   wadamesh `lib_deps` library, or a well-known open-source piece (protocol
   spec + established parser) over a new naive implementation.
2. **Do not invent stubs.** Drawer apps must be real: launch, UI, and the
   underlying work (parse / transfer / render / game tick). Caps and
   documented limits are fine; empty handlers and fake success are not.
3. **Host + patch, do not vendor engines.** Games and large runtimes stay as
   submodules under `third_party/` with a thin AppPage host here and a small
   series under `patches/`. Do not copy engine trees into `firmware/`.
4. **Use libs already in the T-Deck env** when they fit, e.g. **ArduinoJson**
   for JSON (LocalSend), **HTTPClient** / shared `common/http_download.cpp`
   for HTTP(S)→SD, **Lexbor** (TactileBrowser) for HTML, **zstd** for ZIM
   clusters, **esp32-ai** `llm.h` for TinyStories, NeoCalculator
   `ChemDatabase.h` for Elements. Do not hand-roll JSON or a second downloader.
5. **Only write glue when nothing linkable exists.** Example: there is no ESP32
   LocalSend library (official is Flutter; other ports are Rust/TS) — implement
   the [localsend/protocol](https://github.com/localsend/protocol) with
   ArduinoJson + the firmware HTTP stack, not a from-scratch JSON scanner.
6. **Heavy desktop stacks stay out** (full libzim, librime, ONNX mesh-image
   codec). Document the limit; ship a subset or store-and-forward instead of a
   fake port.
7. **Patches:** no pure formatting-only diffs. Rebase with
   `scripts/patches.sh`; keep series slim. nixfmt when editing nix, but
   functional goals beat format churn.

## App map (expected reuse)

| App | Reuse |
|---|---|
| Kiwix | Own ZIM subset + `third_party/zstd`; Reader via wadamesh; `http_download` |
| ESP-AI | `third_party/esp32-ai` + `LLM_DEQ_ROW` patch |
| Browser | TactileBrowser/Lexbor + Duktape |
| LocalSend | Protocol v2 + ArduinoJson + WiFi/HTTPClient |
| Download / catalogue | `common/http_download` |
| Elements | NeoCalculator `ChemDatabase.h` |
| OutRun / Voxel | Submodule engines + hosts in `arcade3d/` / `voxel_flight/` |

## Docs

Keep README / `docs/` aligned with the tree; drop survey fluff. See
`patches/README.md` for series layout.
