#!/usr/bin/env python3
"""Build the AirDrop-enabled reTerminal E1002 firmware variants.

Three firmwares:
  switch — photoframe + exclusive ESP32Drop (IDF)
  coex   — photoframe + Espressif APSTA ch6 dual-protocol experiment (IDF)
  open   — FoA open-MAC concurrent STA + AWDL (Rust/embassy, ESP32)

`all` builds every variant. IDF variants need ESP-IDF 6 + npm; open needs the
Rust esp toolchain (see firmware/reterminal-e1002-open/README.md).
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHOTOFRAME = ROOT / "third_party" / "esp32-photoframe"
OPEN_DIR = ROOT / "firmware" / "reterminal-e1002-open"
BOARD = "seeedstudio_reterminal_e1002"
IDF_VARIANTS = {
    "switch": "sdkconfig.switch",
    "coex": "sdkconfig.coex",
}
# Kept for host tests that iterate IDF-only builds.
VARIANTS = IDF_VARIANTS
ALL_VARIANTS = ("switch", "coex", "open")
REQUIRED_ASSETS = [
    "main/webapp/index.html.gz",
    "main/webapp/icon.svg.gz",
    "main/webapp/assets/index.css.gz",
    "main/webapp/assets/index.js.gz",
    "main/webapp/assets/index2.js.gz",
    "main/webapp/assets/exif-reader.js.gz",
    "main/webapp/assets/browser.js.gz",
    "main/webapp/assets/__vite-browser-external.js.gz",
    "main/splash_data/splash.epdgz",
    "main/splash_data/setup_complete.epdgz",
    "main/splash_data/splash_meta.h",
]
OPEN_TARGET = "xtensa-esp32-none-elf"
OPEN_BIN_NAME = "e1002-open"


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def check_assets():
    missing = [p for p in REQUIRED_ASSETS if not (PHOTOFRAME / p).is_file()]
    if missing:
        sys.exit(
            "missing generated assets:\n  "
            + "\n  ".join(missing)
            + "\nrun: cd third_party/esp32-photoframe && python3 build.py "
            "--step webapp --step splash"
        )


def build_variant(name, build_dir):
    fragment = ROOT / "firmware" / "reterminal-e1002" / IDF_VARIANTS[name]
    out = Path(build_dir).resolve() / f"e1002-{name}"
    out.mkdir(parents=True, exist_ok=True)
    defaults = ";".join(
        str(p)
        for p in (
            PHOTOFRAME / "sdkconfig.defaults",
            PHOTOFRAME / "boards" / f"sdkconfig.defaults.{BOARD}",
            ROOT / "firmware" / "reterminal-e1002" / "sdkconfig.airdrop",
            fragment,
        )
    )
    version = f"airdrop-{name}"
    run(
        [
            "idf.py",
            "-C", PHOTOFRAME,
            "-B", out,
            "-D", "IDF_TARGET=esp32s3",
            "-D", f"SDKCONFIG_DEFAULTS={defaults}",
            "-D", f"SDKCONFIG={out / 'sdkconfig'}",
            "-D", f"FIRMWARE_VERSION={version}",
            "build",
        ]
    )
    return out / "esp32-photoframe.bin"


def package_variant(name, app_path):
    out = app_path.parent
    merged = out / "firmware.bin"
    run(["idf.py", "-C", PHOTOFRAME, "-B", out,
         "merge-bin", "-o", merged])
    write_manifest(name, BOARD, "0x0", out, (app_path, merged), chip="esp32s3")
    return merged


def write_manifest(name, board, flash_offset, out, paths, chip):
    manifest = {
        "variant": name,
        "board": board,
        "chip": chip,
        "flash_offset": flash_offset,
        "artifacts": {
            path.name: {
                "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            for path in paths
            if path.is_file()
        },
    }
    (out / "firmware-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def open_cargo_cmd(build_dir):
    """Return the cargo command used to build the FoA open image (testable)."""
    out = Path(build_dir).resolve() / "e1002-open"
    return [
        "cargo",
        "build",
        "--release",
        "--features", "esp32",
        "--target", OPEN_TARGET,
        "--target-dir", str(out / "cargo-target"),
    ]


def build_open(build_dir):
    if not OPEN_DIR.is_dir():
        sys.exit(f"missing {OPEN_DIR.relative_to(ROOT)}; open FoA tree not present")
    if not shutil.which("cargo"):
        sys.exit(
            "cargo not found; install the Rust esp toolchain "
            "(see firmware/reterminal-e1002-open/README.md)"
        )

    out = Path(build_dir).resolve() / "e1002-open"
    out.mkdir(parents=True, exist_ok=True)
    cmd = open_cargo_cmd(build_dir)
    run(cmd, cwd=OPEN_DIR)

    elf = out / "cargo-target" / OPEN_TARGET / "release" / OPEN_BIN_NAME
    if not elf.is_file():
        sys.exit(f"open build produced no ELF at {elf}")
    elf_out = out / f"{OPEN_BIN_NAME}.elf"
    shutil.copy2(elf, elf_out)

    firmware_bin = out / "firmware.bin"
    artifacts = [elf_out]
    if shutil.which("espflash"):
        run(["espflash", "save-image", "ESP32", str(elf), str(firmware_bin)])
        artifacts.append(firmware_bin)
    else:
        print("note: espflash not found; packaged ELF only "
              "(flash with: espflash flash e1002-open.elf)", flush=True)

    write_manifest("open", "esp32-foa-lab", "0x0", out, artifacts, chip="esp32")
    return elf_out


def prepare_idf(skip_assets):
    if not shutil.which("idf.py"):
        sys.exit("idf.py not found; source ESP-IDF release-v6.0 export.sh first")
    if not (PHOTOFRAME / "main").is_dir():
        sys.exit("third_party/esp32-photoframe is missing; run git submodule update --init")
    if not (ROOT / "third_party/ESP32Drop/ESP32Drop/src/awdl/port/awdl_port_esp32.cpp").is_file():
        sys.exit("third_party/ESP32Drop is missing; run git submodule update --init")
    run([ROOT / "scripts" / "patches.sh", "apply", "esp32-photoframe", "ESP32Drop"])
    if not skip_assets:
        run([sys.executable, "build.py", "--board", BOARD,
             "--step", "webapp", "--step", "splash"], cwd=PHOTOFRAME)
    check_assets()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "variant", nargs="?", default="all",
        choices=["all", "airdrop", *ALL_VARIANTS],
    )
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--skip-assets", action="store_true",
                        help="Reuse generated webapp and splash assets (IDF only)")
    args = parser.parse_args()

    variant = "switch" if args.variant == "airdrop" else args.variant
    names = list(ALL_VARIANTS) if variant == "all" else [variant]
    idf_names = [n for n in names if n in IDF_VARIANTS]
    want_open = "open" in names

    if idf_names:
        prepare_idf(args.skip_assets)
        for name in idf_names:
            bin_path = build_variant(name, args.build_dir)
            merged = package_variant(name, bin_path)
            print(f"{name}: {merged} ({merged.stat().st_size} bytes; flash at 0x0)", flush=True)

    if want_open:
        elf = build_open(args.build_dir)
        print(f"open: {elf} ({elf.stat().st_size} bytes; ESP32 FoA lab image)", flush=True)


if __name__ == "__main__":
    main()
