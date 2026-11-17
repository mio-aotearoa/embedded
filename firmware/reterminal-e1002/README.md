# reTerminal E1002: photoframe + AirDrop

esp32-photoframe as published, plus one ESP-IDF component that receives an
AirDrop transfer from a Mac or iPhone and hands the image to photoframe's own
display flow. ESP32Drop is compiled unmodified (receiver-only); the only
compatibility code is a ~70-line Arduino shim.

## What you get

Hold the green (wake/select, `BOARD_HAL_WAKEUP_KEY`) button while powering the
device on:

1. The firmware skips Wi-Fi and starts AWDL + the AirDrop listener instead.
2. The device appears in the share sheet as **reTerminal** (override with
   `-DAIRDROP_BRIDGE_NAME=...`).
3. Send a JPEG or PNG. It is written to `/storage/images/airdrop-<id>.<ext>`,
   AWDL is torn down, and the image goes through photoframe's normal
   dither/palette pipeline onto the panel.
4. The device goes back to deep sleep.

Without the button held, boot is exactly upstream photoframe: Wi-Fi, HTTP API,
rotation schedule, deep sleep.

## Build

```bash
git submodule update --init --recursive
. $IDF_PATH/export.sh
./firmware/reterminal-e1002/build.sh
```

`build.sh` first runs upstream's `build.py --step webapp --step splash` (the
CMake project embeds a built web UI and generated splash assets that are not
committed), then runs `idf.py` with the upstream defaults plus
`sdkconfig.airdrop`. Node.js/npm is required for that first step.

AirDrop mode always ends in deep sleep, so the Wi-Fi stack is never started in
that boot; AWDL's own `awdl_begin()` performs the NVS/netif/event-loop setup, and
the bridge releases Wi-Fi before sleeping.

To change the window length or trigger pin, add build flags:

```bash
idf.py -C third_party/esp32-photoframe -B build/e1002 \
  -D SDKCONFIG_DEFAULTS="..." \
  -D "CMAKE_C_FLAGS=-DAIRDROP_BRIDGE_WINDOW_MS=60000 -DAIRDROP_BRIDGE_TRIGGER_GPIO=5" build
```

## Why it is a boot mode

AWDL parks the interface on 2.4 GHz channel 6 and keeps a transmit schedule
locked to the availability window; ESP32Drop documents that it cannot share the
radio with `WiFi.begin()`. photoframe is a Wi-Fi device. The firmware therefore
time-multiplexes at boot instead of pretending both can run together.

## What is patched in photoframe

`patches/esp32-photoframe/0002-airdrop-boot-window.patch` only:

* adds `airdrop_bridge` to `main`'s `PRIV_REQUIRES`;
* adds one boot hook before `wifi_manager_init()` that runs the window and
  feeds the file to `display_flow_stream_file()`.

`0001` is a one-line build hook so the upstream CMake project sees
`components/`.

## Verified / not verified

* Patch series applies and reverts cleanly against the pinned photoframe commit
  (checked with `scripts/patches.sh status`).
* The Arduino shim was syntax-checked with host g++ against stub ESP-IDF/
  FreeRTOS headers (not compiled against the real IDF).
* The firmware has **not** been built with ESP-IDF here (toolchain absent), so
  the ESP32Drop sources have not been compiled against this IDF version, and
  the boot hook has not run on hardware.
* The bridge's calls into ESP32Drop and photoframe were checked against the
  pinned headers by name and signature (see `docs/architecture.md`), not by
  compilation.
