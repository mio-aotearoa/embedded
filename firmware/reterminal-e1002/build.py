#!/usr/bin/env python3
"""Build the AirDrop-enabled reTerminal E1002 firmware variants.

Two images are produced from the same source with different sdkconfig
fragments:

  airdrop  AWDL owns the radio during the boot window (shipped behaviour,
           deterministic; Wi-Fi and AirDrop are sequential).
  coex     Experimental: vendor coexistence enabled, both stacks initialised;
           AWDL timing and HTTP throughput are expected to degrade. The port
           still needs the APSTA/session-gating work, so treat this as the
           development seam, not a usable simultaneous mode.

Usage:
  firmware/reterminal-e1002/build.py                 # both variants
  firmware/reterminal-e1002/build.py airdrop         # one variant
  firmware/reterminal-e1002/build.py all --build-dir /tmp/e1002

Requires ESP-IDF (idf.py on PATH) and the pre-built web UI/splash assets
(upstream's `python3 build.py --step webapp --step splash`).
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHOTOFRAME = ROOT / "third_party" / "esp32-photoframe"
BOARD = "seeedstudio_reterminal_e1002"
VARIANTS = {
    "airdrop": "sdkconfig.airdrop",
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
    out = Path(build_dir) / f"e1002-{name}"
    defaults = ";".join(
        str(p)
        for p in (
            PHOTOFRAME / "sdkconfig.defaults",
            PHOTOFRAME / "boards" / f"sdkconfig.defaults.{BOARD}",
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
            "-D", f"FIRMWARE_VERSION={version}",
            "build",
        ]
    )
    return out / "esp32-photoframe.bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", nargs="?", default="all",
                        choices=["all", *VARIANTS])
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    args = parser.parse_args()

    if not (PHOTOFRAME / "main").is_dir():
        sys.exit("third_party/esp32-photoframe is missing; run git submodule update --init")

    run([ROOT / "scripts" / "patches.sh", "apply", "esp32-photoframe"])
    check_assets()

    names = list(VARIANTS) if args.variant == "all" else [args.variant]
    for name in names:
        bin_path = build_variant(name, args.build_dir)
        print(f"{name}: {bin_path} ({bin_path.stat().st_size} bytes)", flush=True)


if __name__ == "__main__":
    main()
