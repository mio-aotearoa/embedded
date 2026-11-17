# meshcore-tdeck-and-photoframe

Two ESP32-S3 firmwares, built almost entirely out of pinned upstream projects:

| Firmware | Hardware | What runs | Upstream code |
|---|---|---|---|
| [`firmware/tdeck-plus`](firmware/tdeck-plus) | LilyGO T-Deck Plus | wadamesh (MeshCore mesh stack + LVGL touch UI) with a Kiwix ZIM reader from SD | `ALLFATHER-BV/wadamesh` (which pins its MeshCore fork), `facebook/zstd` |
| [`firmware/reterminal-e1002`](firmware/reterminal-e1002) | Seeed reTerminal E1002 | esp32-photoframe + AirDrop receive (ESP32Drop) | `aitjcize/esp32-photoframe`, `s-iwaki-d/ESP32Drop` |

Both devices are ESP32-S3 boards with PSRAM, which is what the upstream
projects need.

## Rule for upstream code

Nothing from an upstream project is copied into this repository.

* Upstream projects are **git submodules** under `third_party/`, pinned to a
  commit (`git submodule status`).
* Changes that an upstream project needs are kept as a **patch series** in
  `patches/<project>/`, applied with `scripts/patches.sh`.
* `scripts/patches.sh rebase` moves a submodule to a new upstream commit and
  re-applies the series with three-way merge, so keeping up with upstream is a
  normal git operation rather than a re-port.

```bash
git clone --recurse-submodules <this-repo> && cd <this-repo>
./scripts/patches.sh apply          # apply local series to third_party/*
./scripts/patches.sh status         # what is applied, what would conflict
./scripts/patches.sh rebase wadamesh origin/main   # forward-port
```

Two series exist: esp32-photoframe (a build hook and a boot-mode hook) and
wadamesh (the Kiwix app: build wiring, a drawer tile and a Reader bridge).
ESP32Drop and zstd are used exactly as published, from the submodule working
tree.

## The two designs

**T-Deck Plus** — wadamesh is already a complete firmware: MeshCore radio,
routing, contacts and channels, an LVGL touch UI with an app drawer, a Files
browser and a Reader that renders local HTML from the card. None of that is
reimplemented here. The Kiwix reader (`firmware/tdeck-plus/kiwix/`) is a new
app page compiled into wadamesh's own T-Deck PlatformIO environment by a single
patch: it lists `.zim` archives on the card, searches their title index, writes
the chosen article's HTML to the card and hands it to the firmware's Reader, so
articles get the native scrolling, keyboard/trackball navigation and back bar.
ZIM parsing is the only format code we own (libzim needs a desktop C++17/ICU/XZ
runtime and has no ESP32 target).

**reTerminal E1002** — esp32-photoframe owns boot, storage, image pipeline and
deep sleep. An ESP-IDF component (`components/airdrop_bridge`) compiles
ESP32Drop's receive path unmodified behind a small `Arduino.h` shim
(`components/esp32drop_compat`), because ESP32Drop is an Arduino library whose
core only needs `millis()`. Because AWDL takes the 2.4 GHz radio exclusively,
AirDrop is a boot mode that runs instead of Wi-Fi, then hands the received
file to photoframe's own display flow.

See [`docs/architecture.md`](docs/architecture.md) for the details, the
constraints behind each choice, and the known gaps.

## Building the T-Deck firmware

The base is **ALLFATHER-BV/wadamesh** (`third_party/wadamesh`, pinned by
`.gitmodules`); `firmware/tdeck-plus/build.sh` refuses to build if that
submodule is not the wadamesh checkout. Two entry points:

```bash
# plain Nix: build the firmware (network needed: PlatformIO downloads the
# espressif32 platform, Xtensa toolchain and libraries)
nix run .#firmware            # == firmware/tdeck-plus/build.sh

# or with the toolchain in a shell
git submodule update --init --recursive
nix develop -c firmware/tdeck-plus/build.sh
```

`nix build` itself is hermetic (no network in the Nix sandbox), so it builds
what can be built offline — the ZIM parser's host test:

```bash
nix build            # result/bin/test_zim_archive, prints PASS
nix build .#host-tests
nix build .#tdeck-builder   # the firmware builder script (run: result/bin/tdeck-build)
```

PlatformIO's own downloads cannot happen inside a sandboxed build, and this
flake does not vendor a 1 GB toolchain closure. The builder therefore runs as
an ordinary program (`nix run`) with a project-local `.platformio/` cache.

## Status

The e-paper firmware is complete as described (bridge + two patches). The
T-Deck firmware is now wadamesh plus the Kiwix app: no MeshCore adapter or UI
fork remains in this repository, only the parser and the picker.

There is **no ESP32 toolchain** here: neither firmware has been compiled or
flashed. Verified: both patch series apply/revert cleanly; the bridge's calls
match the pinned ESP32Drop/photoframe headers; the ZIM parser builds and passes
its host test against the pinned zstd sources; the Arduino shim is
syntax-checked with stubs. See `docs/architecture.md` for the exact list.

## Licensing

* wadamesh and its MeshCore fork — GPL-3.0
* esp32-photoframe — GPL-3.0
* ESP32Drop — 0BSD
* zstd — BSD-3-Clause / GPL-2.0 dual

The combined firmwares are therefore GPL-3.0. ESP32Drop's 0BSD terms are
compatible. `third_party/*` keeps each project's own LICENSE file; this
repository's own code is GPL-3.0 (see `LICENSE`).
