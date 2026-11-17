# Mesh image messages on the T-Deck: what can be displayed

The meshcore-open image transport is small and documented: one MeshCore
`GRP_DATA` blob per chunk (163 bytes), a 4-byte header
(`sender_prefix` 2, `img_id` 1, `idx<<4 | total` 1), one metadata byte in
chunk 0 (`aspect 4 | resolution 2 | rate 2`), up to 15 data chunks, and an
optional XOR parity chunk. Reassembly is cheap.

The **codec** is not: upstream reconstructs pixels with an ONNX model bundle
(~1 GB) and peaks above 2 GB of RAM. The ESP32-S3 has 512 KB SRAM and 8 MB
PSRAM, so no amount of firmware work makes on-device reconstruction run.

## Options that fit the hardware

1. **Store + raw view (recommended).** Reassemble the chunks in firmware, save
   `/sd/meshcomod/images/<sender>-<img_id>.mimg`, and show a chat entry with
   metadata (chunks received/total, bytes, aspect/resolution/rate codes) and a
   hex/ASCII preview. The file is decodable by the phone app, which is where
   the model lives. No pixel output on the T-Deck.
2. **Proxy mosaic (honest label).** Render a deterministic 16x16 block mosaic
   from the encoded bytes, labelled "encoded, not the photo". It is not a
   reconstruction; it only shows that a stream arrived and how complete it is.
   Cheap to draw with LVGL, but of little practical value.
3. **Pixels from the codec on a companion.** The companion already decodes and
   can push a downscaled JPEG/PNG back over BLE/USB; the T-Deck then uses its
   existing image viewer. This keeps the model off the device.

## Firmware hook

The receive path lives in the MeshCore core pulled by wadamesh's `lib_deps`
(`meshcomod` tag), not in a submodule this repository patches. Storing chunks
therefore needs either a patch series against a vendored `meshcomod` checkout
(or a fork pinned by tag), plus a UI entry. The transport parser and the SD
writer can be developed and host-tested here independently of that hook.

No code is added for this yet: the parser needs the exact `GRP_DATA` receive
API of the pinned core, and inventing one blind would not compile.
