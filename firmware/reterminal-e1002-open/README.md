# E1002 open firmware (FoA concurrent STA + AWDL)

Faithful **concurrent Wi‑Fi + AWDL** on an open MAC
([Ferris-on-Air](https://github.com/esp32-open-mac/FoA)): `foa_sta` and
`foa_awdl` virtual interfaces share one radio through FoA’s LMAC.

This is firmware **#3** in the E1002 build matrix:

| Firmware | Stack | Role |
| --- | --- | --- |
| `switch` | IDF photoframe + ESP32Drop exclusive | AirDrop **or** Wi‑Fi |
| `coex` | IDF photoframe + Espressif APSTA | ch6 dual-protocol **RX experiment** on the closed blob |
| **`open`** | FoA + foa_sta + foa_awdl | Concurrent STA + AWDL on an **open** MAC |

## Chip target

FoA today supports **ESP32 / ESP32-S2**, not ESP32-S3. The reTerminal E1002 is
S3, so this image is a **lab / companion-radio** build for classic ESP32 until
FoA adds S3. The companion frame codec in `src/companion.rs` matches
`firmware/reterminal-e1002/companion/` for a future UART link into photoframe.

## Build

```bash
# From repo root (also builds switch+coex when you pass all):
./firmware/reterminal-e1002/build.sh open

# Or cargo directly:
cd firmware/reterminal-e1002-open
cargo build --release --features esp32
```

Requires the [Rust esp toolchain](https://esp-rs.github.io/book/installation/index.html)
(`rustup toolchain install esp`) and, for a flashable `firmware.bin`,
[`espflash`](https://github.com/esp-rs/espflash).

Artifacts land in `build/e1002-open/` (`e1002-open.elf`, optional `firmware.bin`,
`firmware-manifest.json`).

Flash:

```bash
espflash flash build/e1002-open/e1002-open.elf
```

## What this cut does / does not

**Does:** bring up FoA LMAC, attach STA + AWDL VIFs, start AWDL, log a dual-protocol
heartbeat, encode companion READY frames.

**Does not:** full AirDrop CPIO/TLS (app layer), photoframe display, ESP32-S3,
or automatic STA association (set `SSID` / `PASSWORD` in `.cargo/config.toml`
when you extend the STA path).

Prefer a **2.4 GHz AP on channel 6** when testing STA + AWDL together — that is
the only AWDL channel on 2.4 GHz.

## License

MIT OR Apache-2.0 (matches FoA). Linking FoA into a product with GPL photoframe
is via the companion split, not a single binary.
