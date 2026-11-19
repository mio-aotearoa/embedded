# reTerminal E1002 firmware

Three firmwares share one entrypoint (`build.sh` / `build.py`):

| Variant | Stack | Behaviour |
| --- | --- | --- |
| `switch` (`airdrop` alias) | IDF photoframe + ESP32Drop exclusive | Normal boot uses Wi‑Fi. Hold green wake/select at power-on for a 180-second AirDrop window, then display the received JPEG/PNG and sleep. Reboot to switch modes. |
| `coex` | IDF photoframe + Espressif APSTA + ESP32Drop | **ch6 2.4 GHz dual-protocol RX experiment** on the closed Wi‑Fi blob. After Wi‑Fi/HTTP startup, if STA is on channel 6, keep ordinary Wi‑Fi (HTTP GETs) and attach AWDL for AirDrop on the same radio. |
| `open` | FoA (`foa_sta` + `foa_awdl`) | **Faithful concurrent Wi‑Fi + AWDL** on an open MAC. Separate Rust image (classic ESP32 lab / companion radio). See [`../reterminal-e1002-open/README.md`](../reterminal-e1002-open/README.md). |

### Coex notes

Requires a provisioned **2.4 GHz router on channel 6**. HTTP GETs and keep-alive
stay up as the Wi‑Fi side of the test; other mutations return 503. Losing
channel 6 ends the window. Failed cleanup reboots and skips the next window
once. One window per boot; TLS allocations remain until reboot.

### Open notes

FoA does not support ESP32-S3 yet; the open image targets **ESP32** and is the
concurrent-radio path (companion frames toward photoframe later). It does not
replace `switch`/`coex` on the panel until FoA adds S3.

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
nix develop   # optional: Node/CMake/Ninja/esptool from the flake
. "$IDF_PATH/export.sh"
./firmware/reterminal-e1002/build.sh          # switch + coex + open
./firmware/reterminal-e1002/build.sh switch
./firmware/reterminal-e1002/build.sh coex
./firmware/reterminal-e1002/build.sh open     # FoA; no IDF required
```

The IDF builder applies both patch series and generates web/splash assets.
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
scripts/patches.sh status esp32-photoframe ESP32Drop
```

Host tests cover IDF build isolation, radio ownership/channel guards, driver
errors, listener shutdown, HTTP mutation gating, and the open cargo command
shape. IDF images have not been flashed or tested on an E1002. For coex, put
the AP on **2.4 GHz channel 6**, poll HTTP during the window, and send a
JPEG/PNG over AirDrop. Dual-protocol success under load is unverified. For
open, prove STA + AWDL heartbeats on a classic ESP32 lab board.
