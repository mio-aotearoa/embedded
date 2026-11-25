# Nix flake for the T-Deck Plus firmware.
#
# The firmware is wadamesh (pinned submodule) plus this repository's Kiwix app,
# wired in by patches/wadamesh/. Nix cannot run PlatformIO inside the build
# sandbox: it downloads the espressif32 platform, the Xtensa toolchain and the
# libraries from the PlatformIO registry, and a sandboxed build has no network.
# So the flake exposes:
#
#   nix build              hermetic host tests: ZIM parser, browser JS, AirDrop
#   nix run .#firmware     build the real firmware (network, uses PlatformIO)
#   nix develop            same toolchain as `nix run .#firmware`, interactive
#
# The builder runs on the checkout it is invoked from, so it sees the submodules
# and can apply the patch series; it never touches the Nix store copy.
{
  description = "T-Deck Plus firmware: wadamesh base + Kiwix ZIM app";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # Pinned exactly like third_party/zstd (git submodule); the parser's host
    # test links the real decompressor, not a stub.
    zstd = {
      url = "github:facebook/zstd/916e8e70b33317d469a41ee837e47317807815c8";
      flake = false;
    };
    # Duktape's git repo ships raw sources only, and the amalgamation is what the
    # firmware builds against. This is the same 2.7.0 tarball scripts/duktape.sh
    # pins by SHA-256 for the device build, so the host test runs the same engine.
    duktape = {
      url = "https://duktape.org/duktape-2.7.0.tar.xz";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      zstd,
      duktape,
      ...
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            inherit system;
            pkgs = import nixpkgs { inherit system; };
          }
        );
    in
    {
      packages = forAllSystems (
        {
          system,
          pkgs,
        }:
        let
          # Prefer the C/C++ toolchain from stdenv so Darwin uses the clang
          # that Nix wires for the platform; fall back to gcc/g++ names below.
          cc = pkgs.stdenv.cc;
          hostTests = pkgs.stdenv.mkDerivation {
            pname = "tdeck-kiwix-host-tests";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = [
              cc
              pkgs.gnumake
            ];
            dontConfigure = true;
            dontFixup = true;
            buildPhase = ''
              runHook preBuild
              mkdir -p build/obj
              for f in ${zstd}/lib/common/*.c ${zstd}/lib/decompress/*.c; do
                $CC -O1 -DZSTD_DISABLE_ASM=1 \
                  -I${zstd}/lib -I${zstd}/lib/common \
                  -c "$f" -o "build/obj/$(basename "$f" .c).o"
              done
              $CXX -std=gnu++17 -O1 -Wall -Wextra \
                -I firmware/tdeck-plus/kiwix -I ${zstd}/lib \
                firmware/tdeck-plus/test/host/test_zim_archive.cpp \
                firmware/tdeck-plus/kiwix/zim_reader.cpp \
                build/obj/*.o -o build/test_zim_archive
              ./build/test_zim_archive
              $CC -std=c11 -O1 -Wall -Wextra \
                -I firmware/reterminal-e1002/companion \
                -c firmware/reterminal-e1002/companion/airdrop_companion.c \
                -o build/obj/airdrop_companion.o
              $CXX -std=gnu++17 -O1 -Wall -Wextra \
                -I firmware/reterminal-e1002/companion \
                firmware/reterminal-e1002/companion/test/test_airdrop_companion.cpp \
                build/obj/airdrop_companion.o -o build/test_airdrop_companion
              ./build/test_airdrop_companion
              DUK=$(find ${duktape} -name duktape.c -path '*/src/duktape.c' | head -1)
              if [ -z "$DUK" ]; then
                echo "error: duktape.c not found in the pinned Duktape input" >&2
                exit 1
              fi
              $CXX -std=gnu++17 -O1 -Wall -Wextra \
                -I firmware/tdeck-plus/browser -I "$(dirname "$DUK")" \
                firmware/tdeck-plus/test/host/test_browser_js.cpp \
                firmware/tdeck-plus/browser/js_engine.cpp \
                "$DUK" -o build/test_browser_js -lm
              ./build/test_browser_js
              runHook postBuild
            '';
            installPhase = ''
              mkdir -p $out/bin
              cp build/test_zim_archive $out/bin/
              cp build/test_airdrop_companion $out/bin/
              cp build/test_browser_js $out/bin/
            '';
            meta = with pkgs.lib; {
              description = "T-Deck host tests: ZIM parser, browser JS, AirDrop (hermetic)";
              license = licenses.gpl3Plus;
              platforms = platforms.unix;
            };
          };

          tdeckBuilder = pkgs.writeShellApplication {
            name = "tdeck-build";
            runtimeInputs = [
              pkgs.platformio
              pkgs.git
              pkgs.python3
              pkgs.gnumake
              pkgs.cacert
              pkgs.curl
              pkgs.which
              pkgs.coreutils
              pkgs.findutils
            ];
            text = ''
              # Run from the repository checkout, not from the store copy: the
              # patch series writes into third_party/wadamesh, and PlatformIO
              # keeps its downloads in .platformio/.
              root="$(pwd)"
              if [ ! -d "$root/firmware/tdeck-plus" ] || [ ! -d "$root/third_party" ]; then
                echo "error: run this from the repository root (firmware/tdeck-plus not found)" >&2
                exit 1
              fi
              if [ ! -e "$root/third_party/wadamesh/platformio.ini" ]; then
                echo "error: submodules are missing; run: git submodule update --init --recursive" >&2
                exit 1
              fi

              cd "$root"
              ./scripts/patches.sh apply wadamesh TactileBrowser TactileBrowserCore \
                esp32-ai esp32s3-arcade-3d voxel-flight
              ./scripts/duktape.sh

              export PLATFORMIO_CORE_DIR="''${PLATFORMIO_CORE_DIR:-$root/.platformio}"
              export PLATFORMIO_SETTING_ENABLE_TELEMETRY=false
              export SSL_CERT_FILE="''${SSL_CERT_FILE:-${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt}"
              mkdir -p "$PLATFORMIO_CORE_DIR"

              echo "==> wadamesh base: $(git -C third_party/wadamesh rev-parse --short HEAD) ($(git -C third_party/wadamesh remote get-url origin))"
              echo "==> PlatformIO core dir: $PLATFORMIO_CORE_DIR"
              cd third_party/wadamesh
              exec pio run -e "''${PIOENV:-LilyGo_TDeck_companion_radio_touch}" "$@"
            '';
            meta = with pkgs.lib; {
              description = "Build the T-Deck Plus firmware (wadamesh + Kiwix)";
              license = licenses.gpl3Plus;
              platforms = platforms.unix;
            };
          };
        in
        {
          default = hostTests;
          host-tests = hostTests;
          tdeck-builder = tdeckBuilder;
        }
      );

      apps = forAllSystems (
        {
          system,
          pkgs,
        }:
        {
          firmware = {
            type = "app";
            program = "${self.packages.${system}.tdeck-builder}/bin/tdeck-build";
          };
          default = self.apps.${system}.firmware;
        }
      );

      devShells = forAllSystems (
        {
          system,
          pkgs,
        }:
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.platformio
              pkgs.git
              pkgs.python3
              pkgs.gnumake
              pkgs.cacert
              pkgs.coreutils
              pkgs.findutils
              pkgs.curl
              pkgs.which
              # E1002 asset generation / flashing helpers. ESP-IDF itself is
              # still installed outside Nix (see firmware/reterminal-e1002/README.md).
              pkgs.nodejs
              pkgs.esptool
              pkgs.cmake
              pkgs.ninja
              # wadamesh's scripts/run-host-tests.sh wants a real g++ (Apple's
              # /usr/bin/g++ is clang); CI runs that suite in this shell.
              pkgs.gcc
              # `nix develop -c nixfmt --check flake.nix`; CI gates on it.
              pkgs.nixfmt
            ];
            shellHook = ''
              export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
              # Expand at shell time — a Nix attr would leave a literal '$PWD/...'.
              export PLATFORMIO_CORE_DIR="''${PLATFORMIO_CORE_DIR:-$PWD/.platformio}"
              echo "T-Deck Plus: nix run .#firmware   or   firmware/tdeck-plus/build.sh"
              echo "E1002: source \$IDF_PATH/export.sh && firmware/reterminal-e1002/build.sh"
              echo "PlatformIO core dir: $PLATFORMIO_CORE_DIR"
            '';
          };
        }
      );

      checks = forAllSystems (
        { system, ... }:
        {
          host-tests = self.packages.${system}.host-tests;
        }
      );

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt);

    };
}
