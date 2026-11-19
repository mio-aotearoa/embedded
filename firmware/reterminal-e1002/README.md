# reTerminal E1002 firmware

| Variant | Behaviour |
| --- | --- |
| `switch` (`airdrop` alias) | Normal boot uses Wi-Fi. Hold green wake/select at power-on for a 180-second AirDrop window, then display the received JPEG/PNG and sleep. Reboot to switch modes. |
| `coex` | After Wi-Fi/HTTP startup, attempt a 180-second AirDrop window alongside the station connection. Requires a provisioned **2.4 GHz router on channel 6**; otherwise continue with Wi-Fi only. |

Coex keeps HTTP browsing/status available, returns HTTP 503 for mutations
except keep-alive, and defers sleep, rotation, startup downloads and buttons.
Losing channel 6 ends AirDrop. Failed cleanup reboots and skips the next window.
One AirDrop window is supported per boot; TLS allocations remain until reboot.

## Build

Requires ESP-IDF 6.0, Node.js/npm and upstream splash dependencies. Both
variants were compiled and linked with **ESP-IDF 6.0.3**
(`c6c28ec49071929fe0a48a258ce16f67b70863b8`), GCC 15.3.0. On macOS (including
Apple Silicon), install ESP-IDF with the [Espressif macOS
instructions](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/linux-macos-setup.html);
`nix develop` supplies Node.js, CMake, Ninja and esptool.

```bash
git submodule update --init --recursive
nix develop   # optional: Node/CMake/Ninja/esptool from the flake
. "$IDF_PATH/export.sh"
./firmware/reterminal-e1002/build.sh          # both variants
./firmware/reterminal-e1002/build.sh switch   # one variant
./firmware/reterminal-e1002/build.sh coex
```

The builder applies both patch series and generates web/splash assets.
`--skip-assets` reuses existing assets; `--build-dir PATH` changes the output
parent. If native splash generation fails, use
[`scripts/pack_splash.py`](../../scripts/pack_splash.py) with ImageMagick and
rebuild with `--skip-assets`. Each variant has its own `sdkconfig`.

## Outputs and flashing

Each `build/e1002-{switch,coex}/` contains:

* `firmware.bin`: merged bootloader, partition table, initial OTA data and app;
  flash at **0x0**.
* `esp32-photoframe.bin`: application only, for OTA.
* `firmware-manifest.json`: image sizes and SHA-256 checksums.

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 build/e1002-switch/firmware.bin
# Select build/e1002-coex/firmware.bin for the concurrent experiment.
```

## Validation

```bash
python3 -m unittest discover -s firmware/reterminal-e1002/test -v
scripts/patches.sh status esp32-photoframe ESP32Drop
```

Host tests cover build isolation, radio ownership/channel guards, driver
errors, listener shutdown and HTTP mutation gating. Both full firmware builds
pass. Neither has been flashed or tested on an E1002. Coex shares one radio;
successful AirDrop discovery/transfers under Wi-Fi traffic remain unverified.
Test JPEG/PNG reception while polling HTTP, then router loss, another router
channel, and timeout without a transfer.
