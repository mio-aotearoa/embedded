# Patch series

Upstream stays in `third_party/` submodules. Changes live here as numbered
patches against the pinned commit.

```
patches/<project>/series
patches/<project>/NNNN-name.patch
patches/<project>/dir          # optional nested submodule path
```

```bash
./scripts/patches.sh apply
./scripts/patches.sh status
./scripts/patches.sh rebase <project> <upstream-ref>
./scripts/patches.sh refresh <project>   # after editing an applied worktree
```

Before `refresh` on wadamesh, restore generated `src/ui-touch/i18n_builtin.h`
(`git checkout -- …`) so the build-time i18n table does not enter the series.
Do not land pure formatting-only diffs in patches.

## wadamesh (`jade-on-mesh/wadamesh-tak`)

* `0001-apps-build-wiring.patch` — the T-Deck env: single-slot partition table,
  include paths and `build_src_filter` for every app plus the arcade/voxel
  engines, `WADAMESH_*` guards, and `CAP_OTA 0` (USB-only updates).
* `0002-app-pages-and-drawer.patch` — `appPageBarCover()` /
  the `appPageBindTextField()` declaration) and the firmware's status helper, the drawer tiles and
  `appLaunch` cases for the apps under `firmware/tdeck-plus/`, trackball/swipe
  steering for the games, the Kiwix → Reader bridge, `wadameshShowAlert`, and
  the system-layer notification chip.
* `0003-reader-fetch-and-progress.patch` — the Reader's own redirect loop (up to
  4 hops, absolute and relative), live fetch-stage labels, the bigger raw
  buffer, and the shared Web font picker.
* `0004-keyboard-ime-and-back.patch` — the page text-field binding
  (`appPageBindTextField()`) and the physical-keyboard routing into it, CJK IME
  hooks for Space / Enter / printable keys, and the
  status-bar back gesture (tap, not touch-down) for every full-screen page.
* `0005-sd-format-menu.patch` — the FAT32 / exFAT chooser behind SD format and
  the deferred format applying the chosen option.
* `0006-wifi-off-on-battery.patch` — Settings → Battery → “Wi‑Fi off on battery”
  (default on). Runtime hold gates `wifiConfigWantsWifi()`; USB clears it, and
  `test/test_touch_prefs_schema.cpp` covers the new trailing field.

## Game engines

* **esp32s3-arcade-3d** `0001` — rename `config`/`utils` to `arcade_*`, manual
  lateral steer, hill gravity on speed (not throttle state), faster auto-throttle
  ramp, flat track run-up. Host: `firmware/tdeck-plus/arcade3d/`.
* **voxel-flight** `0001` — `.ino` → `voxel_flight_game.cpp`, OLED → host RGB565
  at panel size, hold-to-bank. Host + `font5x7.h`: `firmware/tdeck-plus/voxel_flight/`.

## Other series

* **esp32-photoframe** — `EXTRA_COMPONENT_DIRS` for this repo’s `components/`;
  AirDrop boot window → display pipeline.
* **ESP32Drop** — borrow STA for coex.
* **esp-wifi-hal** / **FoA** — ESP32-S3 support.
* **TactileBrowser** (Lexbor via `dir`) — encoding tables `const` (DRAM → flash).
* **TactileBrowserCore** — text style hook; default ink-on-paper colours.
* **esp32-ai** — `LLM_DEQ_ROW` for SD-backed weights.
