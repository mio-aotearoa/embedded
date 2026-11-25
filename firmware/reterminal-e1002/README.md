# reTerminal E1002 firmware

Three firmwares share one entrypoint (`build.sh` / `build.py`):

| Variant | Stack | Behaviour |
| --- | --- | --- |
| `switch` (`airdrop` alias) | IDF photoframe + ESP32Drop exclusive | Normal boot uses Wi‑Fi. Hold green wake/select at power-on for a 180-second AirDrop window, then display the received JPEG/PNG and sleep. Reboot to switch modes. |
| `coex` | IDF photoframe + Espressif APSTA + ESP32Drop | **ch6 2.4 GHz dual-protocol RX experiment** on the closed Wi‑Fi blob. After Wi‑Fi/HTTP startup, if STA is on channel 6, keep ordinary Wi‑Fi (HTTP GETs) and attach AWDL for AirDrop on the same radio. |
| `open` | FoA (`foa_sta` + `foa_awdl`) | **Faithful concurrent Wi‑Fi + AWDL** on an open MAC. Separate Rust image for **ESP32-S3** (in-tree FoA / esp-wifi-hal patches). See [`../reterminal-e1002-open/README.md`](../reterminal-e1002-open/README.md). |

### Coex notes

Requires a provisioned **2.4 GHz router on channel 6**. HTTP GETs and keep-alive
stay up as the Wi‑Fi side of the test; other mutations return 503. Losing
channel 6 ends the window. Failed cleanup reboots and skips the next window
once. One window per boot; TLS allocations remain until reboot.

### Open notes

The open image targets **ESP32-S3** (E1002) via local patches on FoA /
`esp-wifi-hal` (`patches/FoA`, `patches/esp-wifi-hal`). It runs real FoA STA
(associate or scan) + AWDL (peer/service events) and transmits companion
READY/SESSION frames on UART1. Panel integration with photoframe is still
separate from `switch`/`coex`.

## Build

Requires ESP-IDF 6.0, Node.js/npm and upstream splash dependencies for
`switch`/`coex`. The `open` variant needs the Rust esp toolchain instead.
Both IDF variants were compiled and linked with **ESP-IDF 6.0.3**
(`c6c28ec49071929fe0a48a258ce16f67b70863b8`), GCC 15.3.0. On macOS (including
Apple Silicon), install ESP-IDF with the [Espressif macOS
instructions](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/linux-macos-setup.html);
`nix develop` supplies Node.js, CMake, Ninja and esptool.

```bash
git submodule update --init --recursive
export IDF_TOOLS_PATH="$PWD/.toolchains/espressif"
. ./.toolchains/esp-idf/export.sh
nix shell nixpkgs#cmake nixpkgs#ninja -c \
  ./firmware/reterminal-e1002/build.sh         # switch + coex + open
nix shell nixpkgs#cmake nixpkgs#ninja -c \
  ./firmware/reterminal-e1002/build.sh switch
nix shell nixpkgs#cmake nixpkgs#ninja -c \
  ./firmware/reterminal-e1002/build.sh coex
./firmware/reterminal-e1002/build.sh open     # FoA; no IDF required
```

The IDF builder applies the three photoframe patches and ESP32Drop's, then
generates web/splash assets. Use the minimal Nix shell rather than
`nix develop`: the devShell's Python packages can shadow the project-local
ESP-IDF virtualenv.

### Project-local toolchain

`.toolchains/` is gitignored; nothing is installed system-wide.

```bash
git clone --depth 1 --shallow-submodules -b v6.0.3 --recursive \
  https://github.com/espressif/esp-idf.git .toolchains/esp-idf
export IDF_TOOLS_PATH="$PWD/.toolchains/espressif"
.toolchains/esp-idf/install.sh esp32s3
. .toolchains/esp-idf/export.sh        # now idf.py is on PATH
```

`idf.py` also needs `cmake` and `ninja` on PATH (`nix develop -c` supplies
them, or prepend `nix build nixpkgs#cmake --print-out-paths`). Asset generation
wants `npm` plus `qrcode`, `pillow` and `rsvg-convert`:

```bash
python3 -m venv .toolchains/py
.toolchains/py/bin/pip install qrcode pillow
nix shell nixpkgs#librsvg -c bash -c 'cd third_party/esp32-photoframe && \
  ../../.toolchains/py/bin/python build.py --board seeedstudio_reterminal_e1002 \
  --step webapp --step splash'
```
`--skip-assets` reuses existing assets; `--build-dir PATH` changes the output
parent. If native splash generation fails, use
[`scripts/pack_splash.py`](../../scripts/pack_splash.py) with ImageMagick and
rebuild with `--skip-assets`. Each IDF variant has its own `sdkconfig`.

## Outputs and flashing

Each `build/e1002-{switch,coex}/` contains:

* `firmware.bin`: merged bootloader, partition table, initial OTA data and app;
  flash at **0x0**.
* `esp32-photoframe.bin`: application only, for OTA.
* `firmware-manifest.json`: image sizes and SHA-256 checksums.

`build/e1002-open/` contains `e1002-open.elf` (and `firmware.bin` when
`espflash` is installed). Flash with `espflash flash …`.

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 build/e1002-switch/firmware.bin
# coex: build/e1002-coex/firmware.bin
# open: espflash flash build/e1002-open/e1002-open.elf
```

## Validation

```bash
python3 -m unittest discover -s firmware/reterminal-e1002/test -v
scripts/patches.sh status esp32-photoframe ESP32Drop esp-wifi-hal FoA
```

Host tests cover IDF build isolation, radio ownership/channel guards, driver
errors, listener shutdown, HTTP mutation gating, and the open cargo command
shape. IDF images have not been flashed or tested on an E1002. For coex, put
the AP on **2.4 GHz channel 6**, poll HTTP during the window, and send a
JPEG/PNG over AirDrop. Dual-protocol success under load is unverified. For
open, prove STA + AWDL heartbeats on the E1002 (ESP32-S3).
