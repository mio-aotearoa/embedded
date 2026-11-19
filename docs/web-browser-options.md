# Web vs Browser on the T-Deck

Two drawer tiles, on purpose:

| Tile | What it is |
|---|---|
| **Web** | wadamesh Reader: fetch + `htmlToText`, native scroll/keyboard |
| **Browser** | Lexbor via pinned `tactilebrowser_core` (`firmware/tdeck-plus/browser/`) |

Browser adds an LVGL 8.3 `RenderInterface`, HTTP(S) fetch (`setInsecure()`), and
Duktape 2.7.0 for inline scripts (`document.write` / `title` / `console.log`).
No DOM, no events, no external scripts/stylesheets/images.

Lexbor’s encoding tables are `const` (`patches/TactileBrowser/`) so ~760 KB
stays in flash instead of DRAM. Flash budget uses the single-app partition table
(`partitions_tdeck_browser.csv`).

Engine choice in short: TactileBrowser’s core has no `lv_*` calls and builds
under PlatformIO/LVGL 8.3; LVGL 9 engines (eBrowser, Agave) were not adopted.
See [`architecture.md`](architecture.md).
