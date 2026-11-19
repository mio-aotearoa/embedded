# Patch series

Upstream code is never copied into this repository. When an upstream project
needs a change, the change lives here as a numbered patch against the pinned
submodule commit.

```
patches/<project>/series        ordered patch file names, one per line
patches/<project>/NNNN-name.patch
patches/<project>/dir           optional: submodule path when it is not
                               third_party/<project> (nested submodules)
```

Workflow:

```bash
./scripts/patches.sh apply          # after cloning submodules
./scripts/patches.sh status
./scripts/patches.sh rebase esp32-photoframe origin/main
```

Edit upstream files inside `third_party/<project>` only while the series is
applied. To capture an edit, resolve it in the submodule, then use
`./scripts/patches.sh refresh <project>` and move the interesting hunks into the
numbered patch (the generated `9999-worktree.patch` is a scratch file).

A T-Deck build regenerates the vendored `src/ui-touch/i18n_builtin.h` (the
committed copy is stale), so restore it with `git checkout --` before capturing
a patch: otherwise the generated table's ~36,000-line churn lands in the series.
It is build output, not a change of ours.

## wadamesh

The base is `jade-on-mesh/wadamesh-tak` (`third_party/wadamesh`). That fork's
`main` has its own history and newer apps, so the move from
`ALLFATHER-BV/wadamesh` (`3411a8f`) was a three-way re-apply with three manual
resolutions: the reader fetch worker is now the fork's `readerFetchBody()` (our
HTML-cache reset moved into it), the drawer enum/tile table holds the fork's
apps and ours, and the env keeps this repository's USB-only one-slot partition
table instead of the fork's dual 4.5 MB slots.

* `0001-kiwix-esp32ai-periodic-apps.patch` — registers this repository's apps in
  wadamesh's T-Deck environment. Small hunks: the `kiwix_app.h`,
  `esp32ai_app.h` and `browser_app.h` includes, the drawer tiles and
  `appLaunch()` cases for Kiwix, ESP-AI, Elements, LocalSend, Download, Web
  (`APPACT_READER`, upstream text Reader) and Browser (`APPACT_BROWSER` →
  `BrowserApp::launch`), plus an `extern "C" wadameshOpenReaderUrl()` wrapper
  around `openReaderPage()` so Kiwix articles open in the Reader.
  `platformio.ini` gets the include paths, zstd defines and `build_src_filter`
  entries that compile `firmware/tdeck-plus/{kiwix,common,esp32ai,browser}/`
  and the pinned zstd/TactileBrowser pieces, plus `-D WADAMESH_KIWIX=1`,
  `-D WADAMESH_ESP32AI=1` and `-D WADAMESH_BROWSER=1`. Removing the series
  restores the upstream tree exactly. The same patch also wires the SD format
  chooser, live Web/Reader fetch stages in the status line, and
  `readerWebFont()` for the Browser app's CJK faces.

## esp32-photoframe

* `0001-build-allow-external-components.patch` — one line in the root
  `CMakeLists.txt` adding `${CMAKE_CURRENT_LIST_DIR}/../../components` to
  `EXTRA_COMPONENT_DIRS`, so this repository's `components/` are visible to the
  upstream project without moving code into it.
* `0002-airdrop-boot-window.patch` — adds `airdrop_bridge` to `main`'s
  `PRIV_REQUIRES` and a boot hook in `app_main` that runs the AirDrop receive
  window and then feeds the received file through photoframe's own display flow.

## TactileBrowser

* `0001-encoding-tables-const.patch` — makes every table in Lexbor's generated
  `source/lexbor/encoding/multi.c` (and its `multi.h` declarations) `const`. The
  maps are read-only decode tables, but as non-const data they land in
  `.dram0.data`; on the S3 that was 760 KB of writable data and a 613,584 B DRAM
  overflow. As `const` they go to `.flash.rodata` and the browser links. `dir`
  points this series at `TactileBrowser/tactilebrowser_core/lexbor`, the nested
  submodule that actually holds Lexbor, so `patches.sh apply` works unchanged.

## TactileBrowserCore

`third_party/TactileBrowser` itself (not the nested Lexbor repo) carries:

* `0001-text-style` — optional `set_text_style` on `RenderInterface` so headings
  keep a larger/bolder face instead of every label using 14 px.
* `0002-default-ink-on-paper` — default text/background are dark ink on white
  paper (sites assume a light canvas; white-on-transparent looked blank on the
  T-Deck's night chrome).

Its `dir` is `TactileBrowser`, so both apply beside the Lexbor series.

## esp32-ai

* `0001-deq-row-hook.patch` — adds `LLM_DEQ_ROW` so platforms that keep int4
  weights off a linear map (the T-Deck reads them from the SD card) can override
  the two `deq_row` calls in `llm_forward` without forking the rest of
  `runtime/llm.h`.

## esp32s3-arcade-3d

Pinned submodule `third_party/esp32s3-arcade-3d`
(`davidmonterocrespo24/esp32s3-arcade-3d`, branch `road`). The T-Deck AppPage
host and TFT_eSPI canvas shim stay in `firmware/tdeck-plus/arcade3d/`; only the
engine is patched.

* `0001-manual-steer-no-lateral-autopilot.patch` — renames `config.h` /
  `utils.*` to `arcade_config.h` / `arcade_utils.*` (avoids clashing with other
  `config.h` on the T-Deck include path), removes the demo's lateral autopilot,
  and adds `setManualSteer()` so the host can bank left/right while throttle
  stays automatic. Hill gravity is applied to speed (not integrated into the
  throttle state), auto-throttle ramps faster, and random tracks get a short
  flat run-up so the car is not pinned at standstill on a spawn climb.

## voxel-flight

Pinned submodule `third_party/voxel-flight` (`lewtheegg/voxel-flight`). The
AppPage host is `firmware/tdeck-plus/voxel_flight/` (including a bundled
`font5x7.h` formerly pulled from the SparkFun OLED library).

* `0001-wadamesh-host-framebuffer.patch` — renames `voxel-flight.ino` →
  `voxel_flight_game.cpp` and swaps the OLED/DMA path for a host RGB565
  framebuffer at panel size, with optional hold-to-bank steering over the
  upstream autopilot.

## esp-wifi-hal

* `0001-esp32s3-support.patch` — adds an `esp32s3` feature. The official S3 PAC
  has WIFI IRQs but no reverse-engineered MAC register block; the patch reuses
  the S2 WIFI map at `0x6003_3000`, clocks/resets via `APB_CTRL`, and the
  S2-class MAC init / modem-clock masks.

## FoA

* `0001-esp32s3-support.patch` — wires `esp32s3` through `foa` / `foa_sta` /
  `foa_awdl` (and examples) and points `esp-wifi-hal` at the patched path crate
  under `third_party/esp-wifi-hal`.
