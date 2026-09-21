#!/usr/bin/env bash
#
# build_firmware.sh — build the TTGO-VGA32-COCO emulator in both flavours:
#
#   standalone  A single merged image flashed over USB at offset 0x0
#               (bootloader + partition table + boot_app0 + app in one .bin).
#               ->  TTGO-VGA32-CoCo-<VERSION>-firmware.bin
#
#   bootloader  A BARE app image for ESP32_Bootloader's SD-card menu
#               (https://github.com/ESP-WORKS/ESP32_Bootloader), built with
#               -DBUILD_TARGET=1 so the sketch releases `otadata` at boot.
#               ->  build/sdcard/CoCo/{firmware.bin,version.txt}
#
# The two differ ONLY by that command-line define; no source edit is needed,
# and the standalone build stays byte-for-byte what it always was.
#
# Usage:  tools/build_firmware.sh [VERSION] [TARGET]
#           VERSION  defaults to FIRMWARE_VERSION in config.h
#           TARGET   both (default) | standalone | bootloader
#
#         VERSION_STRING=... overrides the contents of version.txt.
#
# Examples:
#   tools/build_firmware.sh                     # both, version from config.h
#   tools/build_firmware.sh 0.9.0               # both, explicit version
#   tools/build_firmware.sh 0.9.0 bootloader    # SD-card package only
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SKETCH_DIR"

readonly SKETCH="TTGO-VGA32-COCO"
readonly FQBN="esp32:esp32:esp32wrover:PartitionScheme=huge_app"
readonly MENU_NAME="CoCo"                 # SD folder name = bootloader menu entry
readonly OTA0_MAX_BYTES=$((0x2C0000))     # ota_0 in the ESP32_Bootloader table

# config.h is the single source of truth for the version; don't hardcode it here.
config_version="$(sed -n 's/^#define[[:space:]]\+FIRMWARE_VERSION[[:space:]]\+"\(.*\)".*/\1/p' config.h)"
VERSION="${1:-$config_version}"
TARGET="${2:-both}"

case "$TARGET" in
both | standalone | bootloader) ;;
*)
    echo "ERROR: unknown target '$TARGET' (expected: both, standalone, bootloader)" >&2
    exit 1
    ;;
esac

if [[ -z "$VERSION" ]]; then
    echo "ERROR: could not read FIRMWARE_VERSION from config.h; pass a version explicitly" >&2
    exit 1
fi

if [[ "$VERSION" != "$config_version" ]]; then
    echo "WARNING: building as '$VERSION' but config.h says '$config_version'." >&2
    echo "         The About screen and /api/status will report '$config_version'." >&2
fi

# Snapshot the git description BEFORE building anything. The standalone build
# rewrites TTGO-VGA32-CoCo-<VER>-firmware.bin, which is tracked, and ESP32
# images are not byte-reproducible — so asking git after the build would report
# `-dirty` for every release, caused by the build itself rather than by any
# uncommitted source change.
GIT_DESC="$(git -C "$SKETCH_DIR" describe --tags --always --dirty 2>/dev/null || true)"

# Locate ESP32 core tooling — pick the highest installed version, don't hardcode.
ESP_ROOT="$HOME/.arduino15/packages/esp32"
ESPTOOL="$(ls "$ESP_ROOT"/tools/esptool_py/*/esptool.py 2>/dev/null | sort -V | tail -1)"
BOOT_APP0="$(ls "$ESP_ROOT"/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | sort -V | tail -1)"

if [[ -z "$ESPTOOL" || -z "$BOOT_APP0" ]]; then
    echo "ERROR: could not find esptool.py or boot_app0.bin under $ESP_ROOT" >&2
    echo "       Is the esp32:esp32 2.0.x core installed?" >&2
    exit 1
fi

# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

# compile <output-dir> [extra arduino-cli args...]
compile() {
    local outdir="$1"
    shift
    rm -rf "$outdir"
    # --port none: with a default_port configured, arduino-cli probes the board
    # for metadata even on a plain compile and fails when it is unplugged.
    # Building should never require the hardware attached.
    arduino-cli compile \
        --fqbn "$FQBN" \
        --port none \
        --output-dir "$outdir" \
        "$@" \
        "$SKETCH_DIR"
}

# require_artifacts <dir> <file>...
require_artifacts() {
    local dir="$1"
    shift
    for f in "$@"; do
        [[ -f "$dir/$f" ]] || { echo "ERROR: missing build artifact $dir/$f" >&2; exit 1; }
    done
}

# Every ESP32 app image starts with 0xE9. A merged flash image starts with 0xFF
# padding instead — shipping one of those to the bootloader silently bricks the
# menu entry, so check before staging.
assert_bare_app_image() {
    local f="$1" magic
    magic="$(head -c1 "$f" | od -An -tx1 | tr -d ' \n')"
    if [[ "$magic" != "e9" ]]; then
        echo "ERROR: $f starts with 0x$magic, not 0xE9 — not a bare app image" >&2
        exit 1
    fi
}

# Prove -DBUILD_TARGET=1 actually reached the compiled sketch. The failure mode
# is silent: a define that never landed produces a standalone build under a
# bootloader name, which boots straight past the menu forever. Checking for
# esp_partition_* in `nm` is NOT enough — the core links those symbols into
# every build — so look for the calls inside setup() itself.
assert_otadata_erase() {
    local elf="$1" want="$2" tcbin addr calls
    tcbin="$(ls -d "$ESP_ROOT"/tools/xtensa-esp32-elf-gcc/*/bin 2>/dev/null | sort -V | tail -1)"
    if [[ -z "$tcbin" || ! -x "$tcbin/xtensa-esp32-elf-nm" ]]; then
        echo "    (skipped otadata-erase check: xtensa toolchain not found)"
        return 0
    fi

    addr="$("$tcbin/xtensa-esp32-elf-nm" "$elf" | awk '$3=="_Z5setupv" || $3=="setup" {print $1; exit}')"
    if [[ -z "$addr" ]]; then
        echo "    (skipped otadata-erase check: setup() symbol not found in $elf)"
        return 0
    fi

    calls="$("$tcbin/xtensa-esp32-elf-objdump" -d \
        --start-address="0x$addr" \
        --stop-address="$((0x$addr + 0xc0))" "$elf" 2>/dev/null \
        | grep -oE 'esp_partition_(find_first|erase_range)' || true)"

    if [[ "$want" == "present" ]]; then
        if ! grep -q esp_partition_find_first <<<"$calls" ||
           ! grep -q esp_partition_erase_range <<<"$calls"; then
            echo "ERROR: bootloader build does not erase otadata at the top of setup()." >&2
            echo "       -DBUILD_TARGET=1 did not reach the sketch; it would never" >&2
            echo "       return to the ESP32_Bootloader menu." >&2
            exit 1
        fi
        echo "    otadata erase present in setup()  ✓"
    else
        if grep -q esp_partition_ <<<"$calls"; then
            echo "ERROR: standalone build erases otadata in setup() — BUILD_TARGET leaked." >&2
            exit 1
        fi
        echo "    otadata erase absent from setup() ✓"
    fi
}

# --------------------------------------------------------------------------
# standalone — merged image, flashed at 0x0
# --------------------------------------------------------------------------

build_standalone() {
    local outdir="build/standalone"
    local out_name="TTGO-VGA32-CoCo-${VERSION}-firmware.bin"

    echo "==> [standalone] Compiling ($FQBN)"
    compile "$outdir"
    require_artifacts "$outdir" \
        "$SKETCH.ino.bin" "$SKETCH.ino.bootloader.bin" "$SKETCH.ino.partitions.bin"

    assert_otadata_erase "$outdir/$SKETCH.ino.elf" absent

    echo "==> [standalone] Merging into $out_name (flash @ 0x0)"
    python3 "$ESPTOOL" --chip esp32 merge_bin \
        -o "$SKETCH_DIR/$out_name" \
        --flash_mode dio \
        --flash_freq 80m \
        --flash_size 4MB \
        0x1000  "$outdir/$SKETCH.ino.bootloader.bin" \
        0x8000  "$outdir/$SKETCH.ino.partitions.bin" \
        0xe000  "$BOOT_APP0" \
        0x10000 "$outdir/$SKETCH.ino.bin"

    STANDALONE_OUT="$SKETCH_DIR/$out_name"
    STANDALONE_SIZE="$(stat -c%s "$STANDALONE_OUT")"
}

# --------------------------------------------------------------------------
# bootloader — bare app image staged for the SD card
# --------------------------------------------------------------------------

build_bootloader() {
    local outdir="build/bootloader"
    local stage="build/sdcard/$MENU_NAME"
    local app="$outdir/$SKETCH.ino.bin"

    echo "==> [bootloader] Compiling with -DBUILD_TARGET=1"
    # The ESP32 core has separate hooks for C and C++, both empty by default.
    # Set both, so any .c file added later still sees the define.
    compile "$outdir" \
        --build-property compiler.cpp.extra_flags=-DBUILD_TARGET=1 \
        --build-property compiler.c.extra_flags=-DBUILD_TARGET=1
    require_artifacts "$outdir" "$SKETCH.ino.bin"

    assert_otadata_erase "$outdir/$SKETCH.ino.elf" present
    assert_bare_app_image "$app"

    local size
    size="$(stat -c%s "$app")"
    if (( size > OTA0_MAX_BYTES )); then
        echo "ERROR: app image is $((size / 1024)) KB; ota_0 holds $((OTA0_MAX_BYTES / 1024)) KB" >&2
        exit 1
    fi

    local version_string="${VERSION_STRING:-}"
    if [[ -z "$version_string" ]]; then
        # version.txt is the ONLY thing the bootloader uses to decide whether to
        # reflash. Fold in `git describe` (captured before the build — see
        # GIT_DESC above) so two builds of the same FIRMWARE_VERSION never share
        # a string and silently skip the reflash.
        version_string="$MENU_NAME.$VERSION${GIT_DESC:+-$GIT_DESC}"
    fi
    case "$version_string" in
    *-dirty)
        echo "WARNING: version '$version_string' has uncommitted changes — do not release it" >&2
        ;;
    esac

    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$app" "$stage/firmware.bin"
    printf '%s\n' "$version_string" > "$stage/version.txt"

    BOOTLOADER_OUT="$SKETCH_DIR/$stage"
    BOOTLOADER_SIZE="$size"
    BOOTLOADER_VERSION="$version_string"
}

# --------------------------------------------------------------------------

STANDALONE_OUT=""; STANDALONE_SIZE=0
BOOTLOADER_OUT=""; BOOTLOADER_SIZE=0; BOOTLOADER_VERSION=""

echo "==> TTGO-VGA32-COCO $VERSION — target: $TARGET"
if [[ "$TARGET" == "both" || "$TARGET" == "standalone" ]]; then
    build_standalone
fi
if [[ "$TARGET" == "both" || "$TARGET" == "bootloader" ]]; then
    build_bootloader
fi

echo
echo "==> Done"
if [[ -n "$STANDALONE_OUT" ]]; then
    echo "  standalone (USB, flash @ 0x0)"
    echo "    $STANDALONE_OUT  ($((STANDALONE_SIZE / 1024)) KB)"
    echo "    esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash 0x0 $(basename "$STANDALONE_OUT")"
fi
if [[ -n "$BOOTLOADER_OUT" ]]; then
    echo "  ESP32_Bootloader (SD card)"
    echo "    $BOOTLOADER_OUT/firmware.bin  ($((BOOTLOADER_SIZE / 1024)) KB of $((OTA0_MAX_BYTES / 1024)) KB)"
    echo "    $BOOTLOADER_OUT/version.txt   $BOOTLOADER_VERSION"
    echo "    copy the '$MENU_NAME' folder to the SD card root:"
    echo "      cp -r $BOOTLOADER_OUT /media/\$USER/<SD>/"
fi
