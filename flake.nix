# Nix flake for the T-Deck Plus firmware.
#
# The firmware is wadamesh (pinned submodule) plus this repository's Kiwix app,
# wired in by patches/wadamesh/. Nix cannot run PlatformIO inside the build
# sandbox: it downloads the espressif32 platform, the Xtensa toolchain and the
# libraries from the PlatformIO registry, and a sandboxed build has no network.
# So the flake exposes:
#
#   nix build              hermetic host test of the ZIM parser (no network)
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
      url = "github:facebook/zstd/f8745da6ff1ad1e7bab384bd1f9d742439278e99";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      zstd,
      ...
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          hostTests = pkgs.stdenv.mkDerivation {
            pname = "tdeck-kiwix-host-tests";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = [
              pkgs.gcc
              pkgs.gnumake
            ];
            dontConfigure = true;
            dontFixup = true;
            buildPhase = ''
              runHook preBuild
              mkdir -p build/obj
              for f in ${zstd}/lib/common/*.c ${zstd}/lib/decompress/*.c; do
                gcc -O1 -DZSTD_DISABLE_ASM=1 \
                  -I${zstd}/lib -I${zstd}/lib/common \
                  -c "$f" -o "build/obj/$(basename "$f" .c).o"
              done
              g++ -std=gnu++17 -O1 -Wall -Wextra \
                -I firmware/tdeck-plus/kiwix -I ${zstd}/lib \
                firmware/tdeck-plus/test/host/test_zim_archive.cpp \
                firmware/tdeck-plus/kiwix/zim_reader.cpp \
                build/obj/*.o -o build/test_zim_archive
              ./build/test_zim_archive
              gcc -std=c11 -O1 -Wall -Wextra \
                -I firmware/reterminal-e1002/companion \
                -c firmware/reterminal-e1002/companion/airdrop_companion.c \
                -o build/obj/airdrop_companion.o
              g++ -std=gnu++17 -O1 -Wall -Wextra \
                -I firmware/reterminal-e1002/companion \
                firmware/reterminal-e1002/companion/test/test_airdrop_companion.cpp \
                build/obj/airdrop_companion.o -o build/test_airdrop_companion
              ./build/test_airdrop_companion
              runHook postBuild
            '';
            installPhase = ''
              mkdir -p $out/bin
              cp build/test_zim_archive $out/bin/
              cp build/test_airdrop_companion $out/bin/
            '';
            meta = with pkgs.lib; {
              description = "ZIM parser host test (hermetic)";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
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
              pkgs.which
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
              ./scripts/patches.sh apply wadamesh

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
              platforms = platforms.linux;
            };
          };
        in
        {
          default = hostTests;
          host-tests = hostTests;
          tdeck-builder = tdeckBuilder;
        }
      );

      apps = forAllSystems (pkgs: {
        firmware = {
          type = "app";
          program = "${self.packages.${pkgs.system}.tdeck-builder}/bin/tdeck-build";
        };
        default = self.apps.${pkgs.system}.firmware;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.platformio
            pkgs.git
            pkgs.python3
            pkgs.gnumake
            pkgs.cacert
          ];
          PLATFORMIO_CORE_DIR = "$PWD/.platformio";
          shellHook = ''
            export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
            echo "T-Deck Plus: ./scripts/patches.sh apply wadamesh && firmware/tdeck-plus/build.sh"
            echo "PlatformIO core dir: $PLATFORMIO_CORE_DIR"
          '';
        };
      });

      checks = forAllSystems (pkgs: {
        host-tests = self.packages.${pkgs.system}.host-tests;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);

    };
}
