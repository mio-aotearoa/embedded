# Agent notes

## Prefer existing / public code over homemade

When adding or changing firmware apps:

1. **Reuse first.** Prefer a pinned `third_party/` submodule, an already-linked
   wadamesh `lib_deps` library, or a well-known open-source piece (protocol
   spec + established parser) over a new naive implementation.
2. **Share common code inside this firmware.** Do not copy the same helper into
   multiple apps. Put shared behaviour under `firmware/tdeck-plus/common/` and
   call it from the apps. Current shared pieces: `http_fetch`, `http_download`,
   `sd_atomic`, `task_spawn`, `psram_alloc`, `game_steer`, `game_canvas`,
   `url_util`, `app_toast`, `cpu_boost`. If you are about to paste a second
   copy of a TLS/redirect/URL/JSON/task-spawn/steer loop, extract it instead.
3. **Do not invent stubs.** Drawer apps must be real: launch, UI, and the
   underlying work (parse / transfer / render / game tick). Caps and
   documented limits are fine; empty handlers and fake success are not.
4. **Host + patch, do not vendor engines.** Games and large runtimes stay as
   submodules under `third_party/` with a thin AppPage host here and a small
   series under `patches/`. Do not copy engine trees into `firmware/`.
5. **Use libs already in the T-Deck env** when they fit, e.g. **ArduinoJson**
   for JSON (LocalSend), **HTTPClient** via `common/http_fetch` /
   `http_download`, **Lexbor** (TactileBrowser) for HTML, **zstd** for ZIM
   clusters, **esp32-ai** `llm.h` for TinyStories, NeoCalculator
   `ChemDatabase.h` for Elements. Do not hand-roll JSON or a second downloader.
6. **Only write glue when nothing linkable exists.** Example: there is no ESP32
   LocalSend library (official is Flutter; other ports are Rust/TS) — implement
   the [localsend/protocol](https://github.com/localsend/protocol) with
   ArduinoJson + the firmware HTTP stack, not a from-scratch JSON scanner.
7. **Heavy desktop stacks stay out** (full libzim, librime, ONNX mesh-image
   codec). Document the limit; ship a subset or store-and-forward instead of a
   fake port.
8. **Patches / nixfmt:** patch series must not include purely formatting-only
   diffs. Rebase with `scripts/patches.sh`; keep series slim. When editing
   `.nix` files, run **nixfmt** in general — that rule can relax when a more
   important goal conflicts (e.g. keeping a patch free of format-only noise).

## App map (expected reuse)

| App | Reuse |
|---|---|
| Kiwix | Own ZIM subset + `third_party/zstd`; Reader; `http_fetch` / `http_download` |
| ESP-AI | `third_party/esp32-ai` + `LLM_DEQ_ROW`; `http_download` |
| Browser | TactileBrowser/Lexbor + Duktape; `http_fetch` |
| LocalSend | Protocol v2 + ArduinoJson + WiFi/HTTPClient |
| Download | `http_download` + `url_util` |
| Elements | NeoCalculator `ChemDatabase.h` |
| OutRun / Voxel | Submodule engines + hosts; `game_canvas` |

## Docs

Keep README / `docs/` aligned with the tree; drop survey fluff. See
`patches/README.md` for series layout.
