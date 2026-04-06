#!/usr/bin/env bash
# build_tamoled.sh — build MicroPython firmware for LilyGO T-AMOLED (ESP32-S3)
#
# Usage:
#   ./build_tamoled.sh [flash|erase-flash|monitor]
#
# Prerequisites:
#   - ESP-IDF v5.x installed and sourced, OR idf.py in PATH
#   - MicroPython source tree at $MPY_DIR (default: ~/micropython)
#   - esptool.py available (pip install esptool)
#   - Device connected via USB, port detected automatically or set $PORT

set -euo pipefail

# ── configurable variables ────────────────────────────────────────────────────
MPY_DIR="${MPY_DIR:-$HOME/micropython}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
WEBDISPLAY_DIR="$(cd "$(dirname "$0")" && pwd)"

# T-AMOLED hardware: ESP32-S3, 16 MB flash, 8 MB octal PSRAM
BOARD="ESP32_GENERIC_S3"
BOARD_VARIANT="FLASH_16M_SPIRAM_OCT"

PORT="${PORT:-}"          # leave empty for auto-detect
BAUD="${BAUD:-460800}"

# ── helpers ───────────────────────────────────────────────────────────────────
die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

require() {
    command -v "$1" >/dev/null 2>&1 || die "'$1' not found. $2"
}

# ── source ESP-IDF if idf.py is not yet in PATH ───────────────────────────────
if ! command -v idf.py >/dev/null 2>&1; then
    [ -f "$IDF_EXPORT" ] || die "ESP-IDF export script not found at $IDF_EXPORT. " \
        "Set IDF_EXPORT= or install ESP-IDF: https://docs.espressif.com/"
    info "Sourcing ESP-IDF from $IDF_EXPORT"
    # shellcheck disable=SC1090
    source "$IDF_EXPORT"
fi

require idf.py   "Install ESP-IDF: https://docs.espressif.com/"
require python3  "Install python3"

[ -d "$MPY_DIR" ] || die "MicroPython directory not found at $MPY_DIR. " \
    "Clone it: git clone https://github.com/micropython/micropython && " \
    "cd micropython && git submodule update --init"

PORT_DIR="$MPY_DIR/ports/esp32"
[ -d "$PORT_DIR" ] || die "ESP32 port not found at $PORT_DIR"

# ── regenerate embedded HTML header ──────────────────────────────────────────
info "Generating csrc/webdisplay_html.h"
python3 "$WEBDISPLAY_DIR/csrc/gen_html_header.py"

# ── build firmware ────────────────────────────────────────────────────────────
ACTION="${1:-build}"

case "$ACTION" in
  build)
    info "Building MicroPython for $BOARD / $BOARD_VARIANT (T-AMOLED ESP32-S3)"
    cd "$PORT_DIR"
    idf.py \
        -DUSER_C_MODULES="$WEBDISPLAY_DIR/csrc/micropython.cmake" \
        -DMICROPY_BOARD="$BOARD" \
        -DMICROPY_BOARD_VARIANT="$BOARD_VARIANT" \
        build
    info "Build complete."
    echo ""
    echo "Firmware: $PORT_DIR/build-${BOARD}-${BOARD_VARIANT}/micropython.bin"
    echo ""
    echo "To flash:  $0 flash"
    echo "To monitor: $0 monitor"
    ;;

  flash)
    info "Flashing firmware to T-AMOLED (port=${PORT:-auto})"
    require esptool.py "Install with: pip install esptool"
    cd "$PORT_DIR"

    PORT_ARG=()
    [ -n "$PORT" ] && PORT_ARG=(--port "$PORT")

    # Erase + write in one step
    idf.py \
        -DUSER_C_MODULES="$WEBDISPLAY_DIR/csrc/micropython.cmake" \
        -DMICROPY_BOARD="$BOARD" \
        -DMICROPY_BOARD_VARIANT="$BOARD_VARIANT" \
        "${PORT_ARG[@]:+${PORT_ARG[@]}}" \
        flash
    info "Flash complete. Hold BOOT and press RESET if the board does not restart."
    ;;

  erase-flash)
    info "Erasing entire flash on T-AMOLED"
    require esptool.py "Install with: pip install esptool"
    PORT_ARG=()
    [ -n "$PORT" ] && PORT_ARG=(--port "$PORT")
    esptool.py "${PORT_ARG[@]:+${PORT_ARG[@]}}" \
        --chip esp32s3 \
        --baud "$BAUD" \
        erase_flash
    info "Flash erased."
    ;;

  monitor)
    info "Opening serial monitor (Ctrl-] to exit)"
    cd "$PORT_DIR"
    PORT_ARG=()
    [ -n "$PORT" ] && PORT_ARG=(-p "$PORT")
    idf.py "${PORT_ARG[@]:+${PORT_ARG[@]}}" monitor
    ;;

  *)
    echo "Usage: $0 [build|flash|erase-flash|monitor]"
    exit 1
    ;;
esac
