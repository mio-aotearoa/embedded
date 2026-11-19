# E1002 open firmware (FoA concurrent STA + AWDL)

Faithful **concurrent Wi‑Fi + AWDL** on an open MAC
([Ferris-on-Air](https://github.com/esp32-open-mac/FoA)): real `foa_sta` and
`foa_awdl` virtual interfaces share one radio through FoA’s LMAC.

This is firmware **#3** in the E1002 build matrix:

| Firmware | Stack | Role |
| --- | --- | --- |
| `switch` | IDF photoframe + ESP32Drop exclusive | AirDrop **or** Wi‑Fi |
| `coex` | IDF photoframe + Espressif APSTA | ch6 dual-protocol **RX experiment** on the closed blob |
| **`open`** | FoA + foa_sta + foa_awdl | Concurrent STA + AWDL on an **open** MAC |

## What runs (not stubs)

* FoA LMAC bring-up with two VIFs
* **AWDL**: `start()` on channel 6, peer/service event loop, IPv6 link-local logged
* **STA**: `connect_by_ssid` + DHCP when `SSID`/`PASSWORD` are set at build time; otherwise continuous BSS scan so STA RX is exercised beside AWDL
* **Companion UART1 TX (GPIO17)**: real `AC_MSG_READY` at boot and `AC_MSG_SESSION_OPEN` when an AirDrop service is discovered (same frame codec as `firmware/reterminal-e1002/companion/`)

AirDrop **app-layer** (CPIO archive / TLS Everyone transfer) is still ESP32Drop’s
job on `switch`/`coex`, or a future OpenDrop-shaped stack on FoA. This image is
the open-MAC concurrent **link** path.

## Chip target

**ESP32-S3** (reTerminal E1002). Upstream FoA / `esp-wifi-hal` only list ESP32
and ESP32-S2; this repo patches both for S3 (`patches/esp-wifi-hal`,
`patches/FoA`) by treating the S3 Wi‑Fi MAC like the S2 map at `0x6003_3000`
and enabling clocks via `APB_CTRL`.

## Build

```bash
# From repo root (applies FoA / esp-wifi-hal patches):
SSID='your-ch6-ap' PASSWORD='secret' ./firmware/reterminal-e1002/build.sh open

# Or:
./scripts/patches.sh apply esp-wifi-hal FoA
cd firmware/reterminal-e1002-open
SSID='your-ch6-ap' PASSWORD='secret' \
  cargo build --release --features firmware,esp32s3 --target xtensa-esp32s3-none-elf
```

Requires the [Rust esp toolchain](https://esp-rs.github.io/book/installation/index.html)
and, for a flashable `firmware.bin`, [`espflash`](https://github.com/esp-rs/espflash).
On NixOS/nix-darwin you can fetch the installer with `nix shell nixpkgs#espup
nixpkgs#rustup` then `espup install -t esp32s3` and `. ~/export-esp.sh`.

Host check for the companion codec (no esp toolchain):

```bash
cd firmware/reterminal-e1002-open && cargo test --lib
```

Artifacts: `build/e1002-open/` (`e1002-open.elf`, optional `firmware.bin`).

```bash
espflash flash build/e1002-open/e1002-open.elf
```

Use a **2.4 GHz AP on channel 6** when associating STA alongside AWDL.
