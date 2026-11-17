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

## What the T-Deck can actually draw

The chunk header and the single metadata byte are enough to draw a card that
tracks the real image, even though the pixels cannot be decoded:

```
chunk 0 body[0] = aspect(4 bits) | resolution(2 bits) | rate(2 bits)
header          = sender_prefix(2) | img_id(1) | idx<<4 | total(1)
```

* **shape**: the 4-bit aspect code maps to the source photo's aspect ratio
  (the exact table is `kImageAspectCodes` in meshcore-open's
  `image_chunk_transport.dart`). Draw the card at that ratio, not a square.
* **size/quality label**: the resolution and rate codes name the codec point
  (`ft32` / `ft16`, 512x512 reconstruction), so the card can say what the
  sender used.
* **progress**: `idx`/`total` plus the presence of the parity chunk
  (`idx == total`) give "7/9 chunks + parity", bytes received, and whether the
  stream is complete or missing a chunk.
* **identity**: `sender_prefix + img_id` makes a stable per-image key for the
  SD file and the thumbnail seed.

A deterministic 12x12 mosaic drawn from the payload itself gives every image a
distinct, stable visual fingerprint: block (x,y) samples byte
`((y*12+x) * payload_len / 144)` and maps it to one of a few grey levels. The
same encoded bytes always produce the same pattern; two different photos look
different; a partially received stream visibly fills in as chunks arrive. It is
labelled "encoded preview - decode on phone", never presented as the photo.

Layout: aspect-correct card + mosaic + progress row + state line, plus an
optional hex sheet for the first 64 bytes. That is representative (shape,
quality, completeness, identity), honest, and costs no codec, model or RAM.
