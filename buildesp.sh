#!/usr/bin/env bash
# build_tamoled.sh — build MicroPython firmware for LilyGO T-AMOLED (ESP32-S3)
#
# Usage:
#   ./build_tamoled.sh [build|flash|erase-flash|monitor|clean]
#
# Prerequisites:
#   - ESP-IDF v5.5.1 installed and sourced, OR idf.py in PATH
#     (MicroPython does NOT support IDF 6.x yet)
#   - MicroPython source tree at $MPY_DIR (default: ~/micropython)
#   - esptool.py available (pip install esptool)
#   - Device connected via USB; set $PORT if auto-detect fails

set -euo pipefail

# ── configurable variables ────────────────────────────────────────────────────
MPY_DIR="${MPY_DIR:-$HOME/micropython}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
WEBDISPLAY_DIR="$(cd "$(dirname "$0")" && pwd)"

# T-AMOLED hardware: ESP32-S3, 16 MB flash, 8 MB octal PSRAM
BOARD="ESP32_GENERIC_S3"
BOARD_VARIANT="SPIRAM_OCT"

# Explicit build directory name — avoids collisions with other board builds
BUILD_DIR_NAME="build-${BOARD}-${BOARD_VARIANT}"

PORT="${PORT:-}"
BAUD="${BAUD:-460800}"

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ── source ESP-IDF if idf.py is not yet in PATH ───────────────────────────────
if ! command -v idf.py >/dev/null 2>&1; then
    [ -f "$IDF_EXPORT" ] || die \
        "ESP-IDF export script not found at $IDF_EXPORT. " \
        "Install ESP-IDF v5.5.1 and set IDF_EXPORT= if needed."
    info "Sourcing ESP-IDF from $IDF_EXPORT"
    # shellcheck disable=SC1090
    source "$IDF_EXPORT"
fi

[ -d "$MPY_DIR" ] || die \
    "MicroPython not found at $MPY_DIR. " \
    "Clone it: git clone https://github.com/micropython/micropython && cd micropython && git submodule update --init"

PORT_DIR="$MPY_DIR/ports/esp32"
BUILD_DIR="$PORT_DIR/$BUILD_DIR_NAME"

[ -d "$PORT_DIR" ] || die "ESP32 port not found at $PORT_DIR"

# ── cmake flags ───────────────────────────────────────────────────────────────
# USER_C_MODULES must be an absolute path and the file must exist at cmake time.
CMAKE_FILE="$WEBDISPLAY_DIR/csrc/micropython.cmake"

idf_cmake_flags="-DUSER_C_MODULES=$CMAKE_FILE -DMICROPY_BOARD=$BOARD -DMICROPY_BOARD_VARIANT=$BOARD_VARIANT"

port_flag=""
[ -n "$PORT" ] && port_flag="-p $PORT"

# ── stale build cache check ───────────────────────────────────────────────────
# If a previous build used a different USER_C_MODULES path (e.g. a failed
# attempt with a different IDF version), CMake caches the old value and ignores
# the -D flag on subsequent runs. Detect this and wipe automatically.
check_stale_build() {
    local cache="$BUILD_DIR/CMakeCache.txt"
    if [ -f "$cache" ]; then
        local cached
        cached=$(grep -s "^USER_C_MODULES" "$cache" | head -1 || true)
        if [ -n "$cached" ] && ! echo "$cached" | grep -qF "$CMAKE_FILE"; then
            info "Stale CMake cache detected (USER_C_MODULES mismatch). Wiping build dir..."
            rm -rf "$BUILD_DIR"
        fi
    fi
}

# ── regenerate embedded HTML header ──────────────────────────────────────────
gen_html() {
    info "Generating csrc/webdisplay_html.h"
    [ -f "$WEBDISPLAY_DIR/csrc/gen_html_header.py" ] || \
        die "gen_html_header.py not found in $WEBDISPLAY_DIR/csrc/"
    python3 "$WEBDISPLAY_DIR/csrc/gen_html_header.py"
}

# ── actions ───────────────────────────────────────────────────────────────────
ACTION="${1:-build}"

case "$ACTION" in
  build)
    gen_html
    check_stale_build
    info "Building MicroPython for $BOARD / $BOARD_VARIANT (T-AMOLED ESP32-S3)"
    cd "$PORT_DIR"
    idf.py $idf_cmake_flags -B "$BUILD_DIR_NAME" build
    info "Build complete."
    echo ""
    echo "Firmware: $BUILD_DIR/micropython.bin"
    echo ""
    echo "To flash:   PORT=/dev/ttyUSB0 $0 flash"
    echo "To monitor: PORT=/dev/ttyUSB0 $0 monitor"
    ;;

  flash)
    gen_html
    check_stale_build
    info "Building + flashing T-AMOLED (port=${PORT:-auto})"
    cd "$PORT_DIR"
    idf.py $idf_cmake_flags -B "$BUILD_DIR_NAME" $port_flag flash
    info "Flash complete."
    info "If board doesn't restart: hold BOOT, press RESET, release BOOT."
    ;;

  erase-flash)
    info "Erasing entire flash on T-AMOLED (port=${PORT:-auto})"
    command -v esptool.py >/dev/null 2>&1 || \
        die "esptool.py not found. Install: pip install esptool"
    esptool.py $port_flag --chip esp32s3 --baud "$BAUD" erase_flash
    info "Flash erased."
    ;;

  monitor)
    info "Opening serial monitor (Ctrl-] to exit)"
    cd "$PORT_DIR"
    idf.py -B "$BUILD_DIR_NAME" $port_flag monitor
    ;;

  clean)
    info "Removing build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    info "Removing generated header"
    rm -f "$WEBDISPLAY_DIR/csrc/webdisplay_html.h"
    info "Clean done."
    ;;

  *)
    echo "Usage: $0 [build|flash|erase-flash|monitor|clean]"
    echo ""
    echo "Variables (prefix on command line):"
    echo "  MPY_DIR=$MPY_DIR"
    echo "  IDF_EXPORT=$IDF_EXPORT"
    echo "  PORT=${PORT:-(auto-detect)}"
    echo "  BAUD=$BAUD"
    exit 1
    ;;
esac
