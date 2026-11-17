#!/usr/bin/env python3
"""Pack OOBE splash screens without process-cli's native `canvas`.

`esp32-photoframe/scripts/generate_splash.py` renders an SVG, converts it with
rsvg-convert and asks process-cli (node-canvas) to dither it into EPDGZ. When
canvas cannot be built (no prebuilt binary, no Cairo toolchain, Node too new),
this script produces the same asset with tools that are usually present:
ImageMagick/Inkscape for rasterising and plain Python for packing.

EPDGZ is simply gzip of the raw 4bpp panel buffer: two pixels per byte, high
nibble = even pixel, one nibble per pixel. The firmware decompresses it into
((width + 1) / 2) * height bytes (see main/splash_screen.c).

Only the Spectra 6-colour boards (e.g. reTerminal E1002) are supported: white
is nibble 1 and black is nibble 0 there. Grayscale boards need the gc16 level
scale instead, which process-cli's dithering handles better.

Usage:
    python3 scripts/pack_splash.py --board seeedstudio_reterminal_e1002
"""

import argparse
import gzip
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHOTOFRAME = os.path.join(REPO, "third_party", "esp32-photoframe")
BOARDS = os.path.join(PHOTOFRAME, "boards", "boards.json")
OUT_DIR = os.path.join(PHOTOFRAME, "main", "splash_data")

WHITE = 1  # EPD_WHITE in main/splash_screen.c (Spectra)
BLACK = 0


def board_info(board_id):
    with open(BOARDS) as f:
        boards = json.load(f)
    if isinstance(boards, dict):
        boards = boards.get("boards", [])
    for b in boards:
        if b.get("id") == board_id:
            return b
    raise SystemExit(f"unknown board: {board_id}")


def svg_for(width, height, subtitle, qr_a, qr_b):
    """Simple branded splash; QR boxes stay white for the firmware to draw into."""
    (ax, ay, asz), (bx, by, bsz) = qr_a, qr_b
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
  <rect width="{width}" height="{height}" fill="#ffffff"/>
  <rect x="0" y="0" width="{width}" height="{height * 0.12}" fill="#5a3a20"/>
  <rect x="{width / 2 - 52}" y="{height * 0.20}" width="104" height="88" rx="10" fill="#f0e4d0" stroke="#5a3a20" stroke-width="5"/>
  <rect x="{width / 2 - 39}" y="{height * 0.20 + 13}" width="78" height="62" rx="5" fill="#e9b584"/>
  <circle cx="{width / 2 + 16}" cy="{height * 0.20 + 31}" r="10" fill="#fef3c7"/>
  <text x="{width / 2}" y="{height * 0.50}" text-anchor="middle" font-family="DejaVu Sans, sans-serif"
        font-size="34" font-weight="bold" fill="#3a2414">ESP Frame</text>
  <text x="{width / 2}" y="{height * 0.57}" text-anchor="middle" font-family="DejaVu Sans, sans-serif"
        font-size="17" fill="#6b503a">{subtitle}</text>
  <rect x="{ax - 6}" y="{ay - 6}" width="{asz + 12}" height="{asz + 12}" fill="#ffffff" stroke="#c9b8a4" stroke-width="2"/>
  <rect x="{bx - 6}" y="{by - 6}" width="{bsz + 12}" height="{bsz + 12}" fill="#ffffff" stroke="#c9b8a4" stroke-width="2"/>
  <text x="{ax + asz / 2}" y="{ay + asz + 26}" text-anchor="middle" font-family="DejaVu Sans, sans-serif"
        font-size="15" fill="#5a3a20">Scan to connect WiFi</text>
  <text x="{bx + bsz / 2}" y="{by + bsz + 26}" text-anchor="middle" font-family="DejaVu Sans, sans-serif"
        font-size="15" fill="#5a3a20">Scan to download app</text>
</svg>"""


def rasterize(svg_text, width, height, png_path):
    """SVG -> grayscale raw bytes, via Inkscape or ImageMagick."""
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        svg_path = f.name
    candidates = []
    if shutil.which("rsvg-convert"):
        candidates.append(["rsvg-convert", "-w", str(width), "-h", str(height), svg_path, "-o", png_path])
    if shutil.which("inkscape"):
        candidates.append(["inkscape", svg_path, "--export-type=png", f"--export-filename={png_path}",
                           f"--export-width={width}", f"--export-height={height}"])
    if shutil.which("convert"):
        candidates.append(["convert", "-background", "white", "-density", "192", svg_path,
                           "-resize", f"{width}x{height}!", png_path])
    if not candidates:
        raise SystemExit("need rsvg-convert, inkscape or ImageMagick convert")
    errors = []
    try:
        for cmd in candidates:
            try:
                subprocess.run(cmd, check=True, capture_output=True)
                break
            except subprocess.CalledProcessError as exc:
                errors.append(f"{cmd[0]}: {exc.stderr.decode(errors='replace').strip()[:200]}")
        else:
            raise SystemExit("no rasteriser worked:\n  " + "\n  ".join(errors))
        gray = subprocess.run(
            ["convert", png_path, "-colorspace", "Gray", "-depth", "8", "gray:-"],
            check=True, capture_output=True,
        ).stdout
    finally:
        os.unlink(svg_path)
    need = width * height
    if len(gray) < need:
        raise SystemExit(f"rasterised {len(gray)} bytes, expected {need}")
    return gray[:need]


def pack(gray, width, height):
    """1 byte per pixel -> 4bpp nibbles, high nibble = even pixel."""
    out = bytearray(((width + 1) // 2) * height)
    for y in range(height):
        row = y * width
        base = y * ((width + 1) // 2)
        for x in range(width):
            out[base + x // 2] |= (BLACK if gray[row + x] < 128 else WHITE) << (4 if x % 2 == 0 else 0)
    return bytes(out)


def write_epdgz(raw, path):
    with gzip.open(path, "wb", compresslevel=9) as f:
        f.write(raw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="seeedstudio_reterminal_e1002")
    ap.add_argument("--out", default=OUT_DIR)
    args = ap.parse_args()

    info = board_info(args.board)
    width, height = info["resolution"]
    if str(info.get("display_type", "")).startswith("gc"):
        raise SystemExit("grayscale boards need process-cli's dithering; not supported here")
    os.makedirs(args.out, exist_ok=True)
    os.makedirs("/tmp/splash-work", exist_ok=True)

    qr_a = (int(width * 0.26), int(height * 0.66), 110)
    qr_b = (int(width * 0.60), int(height * 0.66), 110)

    screens = [
        ("splash", "Scan the QR codes to set up your frame", qr_a, qr_b),
        ("setup_complete", "Scan the QR code to open the web interface", qr_a, qr_b),
    ]
    for name, subtitle, a, b in screens:
        png = f"/tmp/splash-work/{name}.png"
        gray = rasterize(svg_for(width, height, subtitle, a, b), width, height, png)
        raw = pack(gray, width, height)
        path = os.path.join(args.out, f"{name}.epdgz")
        write_epdgz(raw, path)
        print(f"{name}: {width}x{height}, {len(raw)} raw bytes -> {os.path.getsize(path)} EPDGZ")

    meta = os.path.join(args.out, "splash_meta.h")
    with open(meta, "w") as f:
        f.write(
            "// Generated by scripts/pack_splash.py (fallback, no process-cli)\n"
            f"#define SPLASH_WIFI_QR_X {qr_a[0]}\n"
            f"#define SPLASH_WIFI_QR_Y {qr_a[1]}\n"
            f"#define SPLASH_WIFI_QR_SIZE {qr_a[2]}\n"
            f"#define SETUP_COMPLETE_QR_X {qr_b[0]}\n"
            f"#define SETUP_COMPLETE_QR_Y {qr_b[1]}\n"
            f"#define SETUP_COMPLETE_QR_SIZE {qr_b[2]}\n"
        )
    print(f"meta: {meta}")


if __name__ == "__main__":
    main()
