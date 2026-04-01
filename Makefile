# Makefile — webdisplay build helper
# ─────────────────────────────────────────────────────────────
# Assumes MicroPython source tree is at $(MPY_DIR).
# Set it here or override on the command line:
MPY_DIR ?= $(HOME)/micropython

# ─── targets ─────────────────────────────────────────────────
.PHONY: all unix esp32 rp2 clean gen-html help

all: help

help:
	@echo "webdisplay build targets"
	@echo "  make unix      — build unix port with webdisplay C extension"
	@echo "  make esp32     — build ESP32 firmware  (needs IDF env)"
	@echo "  make rp2       — build RP2040 firmware (needs pico-sdk)"
	@echo "  make gen-html  — regenerate csrc/webdisplay_html.h only"
	@echo "  make clean     — remove build artefacts"
	@echo ""
	@echo "For pure-Python (no build needed):"
	@echo "  Copy mpy/webdisplay.py and html/index.html to your device."

gen-html:
	python3 csrc/gen_html_header.py

# ─── unix port ───────────────────────────────────────────────
unix: gen-html
	$(MAKE) -C $(MPY_DIR)/ports/unix \
	    USER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
	    all
	@echo ""
	@echo "Binary: $(MPY_DIR)/ports/unix/micropython"
	@echo "Run:    $(MPY_DIR)/ports/unix/micropython examples/demo.py"

# ─── ESP32 ───────────────────────────────────────────────────
esp32: gen-html
	cd $(MPY_DIR)/ports/esp32 && \
	idf.py -DUSER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
	       -DMICROPY_BOARD=ESP32_GENERIC \
	       build

# ─── RP2040 ──────────────────────────────────────────────────
rp2: gen-html
	$(MAKE) -C $(MPY_DIR)/ports/rp2 \
	    USER_C_MODULES=$(CURDIR)/csrc/micropython.cmake \
	    BOARD=RPI_PICO_W \
	    all

clean:
	rm -f csrc/webdisplay_html.h
	$(MAKE) -C $(MPY_DIR)/ports/unix clean 2>/dev/null || true
