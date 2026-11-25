# meshcore-tdeck-and-photoframe

Two ESP32-S3 firmwares built from pinned upstream submodules plus local patch
series (`patches/`, `scripts/patches.sh`). Nothing upstream is copied into the
tree.

| Firmware | Hardware | What runs |
|---|---|---|
| [`firmware/tdeck-plus`](firmware/tdeck-plus) | LilyGO T-Deck Plus | wadamesh-tak + drawer apps (Kiwix, ESP-AI, Browser, LocalSend, Download, Elements, OutRun, Voxel Flight) |
| [`firmware/reterminal-e1002`](firmware/reterminal-e1002) | Seeed reTerminal E1002 | photoframe + AirDrop (`switch` / `coex`) and FoA open concurrent radio |

```bash
git clone --recurse-submodules <this-repo> && cd <this-repo>
./scripts/patches.sh apply
./scripts/patches.sh status
./scripts/patches.sh rebase wadamesh tak-beta_106.22  # the pinned tag; bump the pin after
```

Details: [`docs/architecture.md`](docs/architecture.md),
[`patches/README.md`](patches/README.md).

## T-Deck Plus

Base: `jade-on-mesh/wadamesh-tak` (`third_party/wadamesh`). Apps live under
`firmware/tdeck-plus/` and are wired in by `patches/wadamesh/` (build filter,
drawer tiles, Reader bridge, Wi‑Fi-off-on-battery). Game engines are submodules
(`esp32s3-arcade-3d`, `voxel-flight`) with small host patches; hosts stay in
this repo.

**Web** = wadamesh Reader (text). **Browser** = Lexbor via pinned
TactileBrowser core (`firmware/tdeck-plus/browser/`). LocalSend send+receive is
ours (protocol v2). Shared HTTP(S)→SD downloads: `common/http_download.cpp`.

```bash
git submodule update --init --recursive
nix run .#firmware
nix run .#firmware -- -t upload
# or
nix develop -c firmware/tdeck-plus/build.sh
```

Nix systems: `x86_64-linux`, `aarch64-linux`, `aarch64-darwin`, `x86_64-darwin`.
Images land in `third_party/out/`. Hermetic host tests: `nix build` →
`result/bin/test_zim_archive`.

## reTerminal E1002

```bash
export IDF_TOOLS_PATH="$PWD/.toolchains/espressif"
. .toolchains/esp-idf/export.sh
nix shell nixpkgs#cmake nixpkgs#ninja -c \
  ./firmware/reterminal-e1002/build.sh switch
```

See [`firmware/reterminal-e1002/README.md`](firmware/reterminal-e1002/README.md).

```bash
nix build .#host-tests \
  && ./result/bin/test_zim_archive \
  && ./result/bin/test_airdrop_companion
```

## Licensing

wadamesh / MeshCore fork / esp32-photoframe / this repo’s own code — GPL-3.0.
ESP32Drop — 0BSD. FoA — MIT OR Apache-2.0. zstd — BSD-3 / GPL-2. See each
`third_party/*/LICENSE`.
