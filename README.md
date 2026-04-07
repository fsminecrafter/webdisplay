# webdisplay

A fast framebuffer display library for MicroPython — renders to a browser window over WebSocket/HTTP on port **6868**. Supports mouse, keyboard, clipboard input. Fullscreenable.

Quick command to build for esp32-s3

```git clone htps://github.com/fsminecrafter/webdisplay.git && cd webdisplay &chmod +xbuildesp.sh && ./buildesp.sh```

## Architecture

```
MicroPython device                    Browser (any device on LAN)
─────────────────                     ─────────────────────────────
webdisplay.begin()  ─── HTTP :6868 ──► serves index.html
                    ─── WebSocket ────► binary draw commands
                    ◄── binary events ─ mouse / keyboard / clipboard
```

**Protocol:** Every draw call sends a tight binary WebSocket frame (1–N bytes). The browser dispatches on the opcode byte and calls Canvas 2D API directly. No JSON, no string parsing.

---

## Option A — Pure Python (fastest to try)

No build needed. Works on any MicroPython with `usocket`.

1. Copy `mpy/webdisplay.py` to your device (e.g. `/webdisplay.py`)  
2. Copy `html/index.html` to the **same directory** as `webdisplay.py`  
3. Connect device to WiFi, note its IP address  
4. Run your script (see examples below)  
5. Open `http://<device-ip>:6868` in a browser  

```python
import webdisplay as wd

wd.begin()
wd.setresolution(720, 480)

while True:
    wd.poll()          # <── call this every loop iteration
    wd.clear(0x001122)
    wd.writebox(10, 10, 100, 50, 0xFF4400, fill=1)
    wd.writetext(10, 10, wd.FONT_8X8, 0xFFFFFF, "Hello!")
```

---

## Option B — C Extension (fastest runtime)

Compiles into the MicroPython firmware. ~3× faster than pure Python for high frame-rate usage.

### Requirements

- MicroPython source tree  
- `python3` in PATH  
- Platform toolchain (see below)  

### Unix port (development / testing)

```bash
git clone https://github.com/micropython/micropython
cd micropython
git submodule update --init

# point to this library
make -C ports/unix USER_C_MODULES=/path/to/webdisplay/csrc/micropython.cmake

# run demo
ports/unix/micropython /path/to/webdisplay/examples/demo.py
```

Or use the convenience Makefile:

```bash
cd webdisplay
MPY_DIR=~/micropython make unix
```

### ESP32 (IDF 5.x)

```bash
# install ESP-IDF per https://docs.espressif.com/
. ~/esp/esp-idf/export.sh

cd micropython/ports/esp32
idf.py \
  -DUSER_C_MODULES=/path/to/webdisplay/csrc/micropython.cmake \
  -DMICROPY_BOARD=ESP32_GENERIC \
  build flash
```

Or:

```bash
MPY_DIR=~/micropython make esp32
```

### RP2040 / Pico W

```bash
# install pico-sdk, set PICO_SDK_PATH
cd micropython/ports/rp2
make \
  USER_C_MODULES=/path/to/webdisplay/csrc/micropython.cmake \
  BOARD=RPI_PICO_W
```

---

## API Reference

### Lifecycle

```python
wd.begin(port=6868)     # start HTTP+WS server; returns True/False
wd.poll()               # process network I/O + fire callbacks — call every loop
wd.setresolution(w, h)  # resize canvas (default 640×480)
```

### Drawing

All drawing functions return `None` (or `None` on failure — same value, check connection count if needed).

```python
wd.clear(rgb=0)                          # fill canvas
wd.writepixel(x, y, rgb)
wd.writeline(x0, y0, x1, y1, rgb)
wd.writebox(x, y, w, h, rgb, fill=0)
wd.writecircle(cx, cy, r, rgb, fill=0)
wd.writetriangle(x0,y0,x1,y1,x2,y2, rgb, fill=0)
wd.writetext(x, y, font, rgb, text)      # font: FONT_8X8, FONT_16X16, FONT_MONO
wd.writebmp(x, y, w, h, data)            # data: bytes/bytearray of RGB888 pixels
```

`rgb` is always a 24-bit integer: `0xRRGGBB`.

**Built-in fonts:**

| Constant       | Size    | Description           |
|----------------|---------|-----------------------|
| `wd.FONT_8X8`  | 8×8 px  | IBM CP437 pixel font  |
| `wd.FONT_16X16`| 16×16px | 2× scaled version     |
| `wd.FONT_MONO` | 14px    | Browser system mono   |

### Input — polling

```python
x   = wd.getmouse(wd.X_MOUSE)   # int, canvas coords
y   = wd.getmouse(wd.Y_MOUSE)
lmb = wd.getmouse(wd.L_CLICK)   # 0 or 1
rmb = wd.getmouse(wd.R_CLICK)
mmb = wd.getmouse(wd.M_CLICK)
key = wd.getmouse(wd.KEY)        # keyCode | modifier bits
```

Modifier bits in key value:
- `0x10000` — Shift
- `0x20000` — Ctrl
- `0x40000` — Alt

### Input — callbacks

```python
def on_click(state):
    if state: print("left button down")

wd.setcallbackfunc(wd.L_CLICK, on_click)
# first argument is always the new value
```

Constants: `X_MOUSE`, `Y_MOUSE`, `L_CLICK`, `R_CLICK`, `M_CLICK`, `KEY`

### Clipboard

```python
wd.setclipboard("text")   # write to browser clipboard
text = wd.getclipboard()  # read what browser last sent

# Browser → device: click the "📋 Sync Clipboard" button in the HUD,
# or call sendClipboard() from JS console.
```

---

## Browser UI

- **HUD** (top bar): auto-hides after 3 s, reappears on mouse/key  
- **⛶ Fullscreen**: enter fullscreen; also works with double-click  
- **📋 Sync Clipboard**: pushes browser clipboard to device  
- **FPS counter**: shows draw frames/sec received  
- Canvas scales to fit window while keeping pixel-perfect rendering  

## File layout

```
webdisplay/
├── csrc/
│   ├── webdisplay.c          C extension (MicroPython module)
│   ├── webdisplay_ws.h       WebSocket framing (SHA-1, base64, RFC 6455)
│   ├── webdisplay_html.h     Auto-generated: embedded HTML (run gen_html_header.py)
│   ├── gen_html_header.py    Converts html/index.html → webdisplay_html.h
│   └── micropython.cmake     CMake fragment for USER_C_MODULES
├── mpy/
│   └── webdisplay.py         Pure-Python fallback (no build needed)
├── html/
│   └── index.html            Browser client (served at :6868/)
├── examples/
│   ├── demo.py               Full feature demo
│   └── minimal.py            Minimal example
├── Makefile                  Build helper
└── README.md                 This file
```

---

## License

GPL
