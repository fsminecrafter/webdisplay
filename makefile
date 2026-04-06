# Makefile — webdisplay build helper
# ─────────────────────────────────────────────────────────────
# Assumes MicroPython source tree is at $(MPY_DIR).
# Set it here or override on the command line:

MPY_DIR ?= $(HOME)/micropython

# ── LilyGO T-AMOLED ESP32-S3 settings ────────────────────────
# 16 MB flash, 8 MB octal PSRAM — matches the RM67162 AMOLED board
TAMOLED_BOARD         := ESP32_GENERIC_S3
TAMOLED_BOARD_VARIANT := FLASH_16M_SPIRAM_OCT
TAMOLED_PORT          ?=           # e.g. /dev/ttyUSB0 or COM3 (leave empty = auto)
TAMOLED_BAUD          ?= 460800

# ── helpers ───────────────────────────────────────────────────
_idf_flags = \
	-DUSER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
	-DMICROPY_BOARD=$(TAMOLED_BOARD) \
	-DMICROPY_BOARD_VARIANT=$(TAMOLED_BOARD_VARIANT)

_port_flag = $(if $(TAMOLED_PORT),-p $(TAMOLED_PORT),)

# ── targets ───────────────────────────────────────────────────
.PHONY: all unix esp32 esp32s3-tamoled tamoled-flash tamoled-erase tamoled-monitor rp2 clean gen-html help

all: help

help:
	@echo "webdisplay build targets"
	@echo " make unix              — build unix port with webdisplay C extension"
	@echo " make esp32             — build ESP32 firmware (needs IDF env)"
	@echo " make esp32s3-tamoled   — build firmware for LilyGO T-AMOLED (ESP32-S3)"
	@echo " make tamoled-flash     — build + flash T-AMOLED (set TAMOLED_PORT= if needed)"
	@echo " make tamoled-erase     — erase entire flash on T-AMOLED"
	@echo " make tamoled-monitor   — open serial monitor for T-AMOLED"
	@echo " make rp2               — build RP2040 firmware (needs pico-sdk)"
	@echo " make gen-html          — regenerate csrc/webdisplay_html.h only"
	@echo " make clean             — remove build artefacts"
	@echo ""
	@echo "Variables (override on command line):"
	@echo "  MPY_DIR=$(MPY_DIR)"
	@echo "  TAMOLED_BOARD=$(TAMOLED_BOARD)"
	@echo "  TAMOLED_BOARD_VARIANT=$(TAMOLED_BOARD_VARIANT)"
	@echo "  TAMOLED_PORT=$(if $(TAMOLED_PORT),$(TAMOLED_PORT),(auto-detect))"
	@echo ""
	@echo "For pure-Python (no build needed):"
	@echo "  Copy mpy/webdisplay.py and html/index.html to your device."

gen-html:
	python3 csrc/gen_html_header.py

# ── unix port ─────────────────────────────────────────────────
unix: gen-html
	$(MAKE) -C $(MPY_DIR)/ports/unix \
		USER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
		all
	@echo ""
	@echo "Binary: $(MPY_DIR)/ports/unix/micropython"
	@echo "Run:    $(MPY_DIR)/ports/unix/micropython examples/demo.py"

# ── ESP32 (generic) ──────────────────────────────────────────
esp32: gen-html
	cd $(MPY_DIR)/ports/esp32 && \
		idf.py -DUSER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
		       -DMICROPY_BOARD=ESP32_GENERIC \
		       build

# ── LilyGO T-AMOLED (ESP32-S3, 16 MB flash, 8 MB octal PSRAM) ───
#
# Board: $(TAMOLED_BOARD) / variant: $(TAMOLED_BOARD_VARIANT)
# QSPI AMOLED display driven via webdisplay C extension over WebSocket.
#
# Prerequisites:
#   1. ESP-IDF v5.x sourced:  . ~/esp/esp-idf/export.sh
#   2. MicroPython cloned:    git clone https://github.com/micropython/micropython
#                             cd micropython && git submodule update --init
#   3. esptool installed:     pip install esptool
# ──────────────────────────────────────────────────────────────
esp32s3-tamoled: gen-html
	@echo "Building for LilyGO T-AMOLED ($(TAMOLED_BOARD)/$(TAMOLED_BOARD_VARIANT))"
	cd $(MPY_DIR)/ports/esp32 && \
		idf.py $(_idf_flags) build
	@echo ""
	@echo "Firmware: $(MPY_DIR)/ports/esp32/build-$(TAMOLED_BOARD)-$(TAMOLED_BOARD_VARIANT)/micropython.bin"
	@echo "Flash:    make tamoled-flash [TAMOLED_PORT=/dev/ttyUSB0]"

tamoled-flash: gen-html
	@echo "Building + flashing T-AMOLED (port=$(if $(TAMOLED_PORT),$(TAMOLED_PORT),auto))"
	cd $(MPY_DIR)/ports/esp32 && \
		idf.py $(_idf_flags) $(if $(TAMOLED_PORT),-p $(TAMOLED_PORT),) flash

tamoled-erase:
	@echo "Erasing flash on T-AMOLED (port=$(if $(TAMOLED_PORT),$(TAMOLED_PORT),auto))"
	esptool.py $(if $(TAMOLED_PORT),--port $(TAMOLED_PORT),) \
		--chip esp32s3 \
		--baud $(TAMOLED_BAUD) \
		erase_flash

tamoled-monitor:
	cd $(MPY_DIR)/ports/esp32 && \
		idf.py $(if $(TAMOLED_PORT),-p $(TAMOLED_PORT),) monitor

# ── RP2040 ────────────────────────────────────────────────────
rp2: gen-html
	$(MAKE) -C $(MPY_DIR)/ports/rp2 \
		USER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
		BOARD=RPI_PICO_W \
		all

# ── clean ─────────────────────────────────────────────────────
clean:
	rm -f csrc/webdisplay_html.h
	$(MAKE) -C $(MPY_DIR)/ports/unix clean 2>/dev/null || true
	rm -rf $(MPY_DIR)/ports/esp32/build-$(TAMOLED_BOARD)-$(TAMOLED_BOARD_VARIANT) 2>/dev/null || true
