#!/usr/bin/env python3
"""Build the AirDrop-enabled reTerminal E1002 firmware variants.

Builds isolated switch (alias: airdrop) and experimental coex images.
By default, prepares upstream webapp/splash assets and builds both variants.
Requires ESP-IDF, npm, and initialised submodules.

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
BOARD = "seeedstudio_reterminal_e1002"
VARIANTS = {
    "switch": "sdkconfig.switch",
    "coex": "sdkconfig.coex",
}
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
    fragment = ROOT / "firmware" / "reterminal-e1002" / VARIANTS[name]
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
    manifest = {
        "variant": name,
        "board": BOARD,
        "flash_offset": "0x0",
        "artifacts": {
            path.name: {
                "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            for path in (app_path, merged)
        },
    }
    (out / "firmware-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return merged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", nargs="?", default="all",
                        choices=["all", "airdrop", *VARIANTS])
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--skip-assets", action="store_true",
                        help="Reuse generated webapp and splash assets")
    args = parser.parse_args()

    if not shutil.which("idf.py"):
        sys.exit("idf.py not found; source ESP-IDF release-v6.0 export.sh first")

    if not (PHOTOFRAME / "main").is_dir():
        sys.exit("third_party/esp32-photoframe is missing; run git submodule update --init")

    if not (ROOT / "third_party/ESP32Drop/ESP32Drop/src/awdl/port/awdl_port_esp32.cpp").is_file():
        sys.exit("third_party/ESP32Drop is missing; run git submodule update --init")

    run([ROOT / "scripts" / "patches.sh", "apply", "esp32-photoframe", "ESP32Drop"])
    if not args.skip_assets:
        run([sys.executable, "build.py", "--board", BOARD,
             "--step", "webapp", "--step", "splash"], cwd=PHOTOFRAME)
    check_assets()

    variant = "switch" if args.variant == "airdrop" else args.variant
    names = list(VARIANTS) if variant == "all" else [variant]
    for name in names:
        bin_path = build_variant(name, args.build_dir)
        merged = package_variant(name, bin_path)
        print(f"{name}: {merged} ({merged.stat().st_size} bytes; flash at 0x0)", flush=True)


if __name__ == "__main__":
    main()
