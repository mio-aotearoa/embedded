# Patch series

Upstream code is never copied into this repository. When an upstream project
needs a change, the change lives here as a numbered patch against the pinned
submodule commit.

```
patches/<project>/series        ordered patch file names, one per line
patches/<project>/NNNN-name.patch
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

## wadamesh

* `0001-kiwix-app.patch` — registers this repository's Kiwix app in wadamesh's
  T-Deck environment. Five small hunks: `include/kiwix_app.h`, one
  `APPACT_KIWIX` drawer tile (gated on `CAP_FILESYSTEM`), its launch case in
  `appLaunch()`, and an `extern "C" wadameshOpenReaderUrl()` wrapper around the
  firmware's existing `openReaderPage()` so articles are rendered by the
  built-in Reader. `platformio.ini` gets the include paths, zstd defines and
  `build_src_filter` entries that compile `firmware/tdeck-plus/kiwix/` and the
  pinned zstd decompressor into the env, plus `-D WADAMESH_KIWIX=1` so every
  hook is compiled only in that env. No wadamesh source is modified in
  substance; removing the series restores the upstream tree exactly.

## esp32-photoframe

* `0001-build-allow-external-components.patch` — one line in the root
  `CMakeLists.txt` adding `${CMAKE_CURRENT_LIST_DIR}/../../components` to
  `EXTRA_COMPONENT_DIRS`, so this repository's `components/` are visible to the
  upstream project without moving code into it.
* `0002-airdrop-boot-window.patch` — adds `airdrop_bridge` to `main`'s
  `PRIV_REQUIRES` and a boot hook in `app_main` that runs the AirDrop receive
  window and then feeds the received file through photoframe's own display flow.
