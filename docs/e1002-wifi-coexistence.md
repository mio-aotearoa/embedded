# E1002: AirDrop (AWDL) and Wi-Fi at the same time

## The constraint is the radio, not the driver

The ESP32-S3 has one 2.4 GHz front end. AWDL needs the radio parked on the
social channel (channel 6) in 16 ms availability windows with tight timing;
normal Wi-Fi needs the same radio for beacons, DHCP, TLS and HTTP. Any
"simultaneous" solution is therefore time-sharing. The question is who
schedules the sharing.

ESP32Drop's port states the limit plainly: it owns the radio exclusively
("cannot coexist with WiFi.begin()") and runs at 100 % lock duty cycle while a
session is active. The vendor stack can be put into `WIFI_MODE_APSTA`, but its
MAC decides when the STA and the AWDL-ish AP get the radio. It has no concept
of AWDL availability windows, so the scheduler cannot honour them.

## What the Rust stack (Ferris-on-Air / FoA) changes

FoA is an open 802.11 stack in async Rust from the esp32-open-mac project. It
already implements station mode, AP mode (in the C stack), Apple AWDL
(`foa_awdl`) and partial 802.11s mesh. Because the MAC is open, its scheduler
can interleave AWDL windows and STA service periods in one place: that is the
one thing the closed MAC cannot do.

It is not a drop-in for this firmware:

* it targets the original ESP32; there is no ESP32-S3 port;
* it is a bare-metal `no_std`/Embassy stack, not an ESP-IDF component, so
  photoframe's C app, lwIP integration, HTTP server, TLS, provisioning and the
  web UI would all have to be rebuilt on top of it;
* it is experimental and not Wi-Fi certified.

Porting the E1002 app to FoA is a rewrite, not a patch. It is the right
long-term answer only if "AWDL and Wi-Fi at once" becomes a core requirement
and someone owns that port.

## Options, in order of cost

1. **Keep the boot-time AirDrop window (current design).** Deterministic,
   already implemented, no timing compromise. Wi-Fi and AirDrop are sequential.
2. **Experimental same-channel APSTA.** Patch the ESP32Drop port to join an
   already-initialised Wi-Fi stack instead of calling `esp_wifi_init`, force
   APSTA, pin the STA to channel 6, enable coex, and gate photoframe's HTTP
   fetches while an AWDL session is active. This can work for short transfers
   but AWDL timing and HTTP throughput will both degrade; packet loss on the
   TLS session is a real risk. It belongs behind a build flag, not as default.
3. **A second radio.** Put ESP32Drop on a small companion (ESP32-C6/H2) or add
   a second Wi-Fi module over SPI/UART. This is the only way to get genuine
   concurrency without rewriting the stack, at the cost of an extra part and a
   transport protocol.
4. **FoA port.** Full control of the schedule, but a rewrite of the whole
   application and no S3 support today.

Recommendation: keep (1) as the shipped behaviour, prototype (2) behind
`-D CONFIG_AIRDROP_COEX=1` only if users need Wi-Fi during a session, and treat
(3) as the hardware answer if simultaneous use becomes mandatory. FoA is worth
tracking, not adopting, until it has an S3 port and an ESP-IDF integration.
