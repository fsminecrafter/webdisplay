"""
demo.py  —  webdisplay demo
Works with both the C extension (import webdisplay) and the pure-Python
fallback (copy mpy/webdisplay.py as webdisplay.py to sys.path).

Run on device:
    import demo   (after copying to device)

Run on unix micropython:
    ./micropython examples/demo.py
"""
import webdisplay as wd
import time, math

# ── setup ──────────────────────────────────────────────────────
wd.begin()
wd.setresolution(720, 480)

# ── input callbacks ────────────────────────────────────────────
mx, my = [360], [240]

def on_mouse_x(x): mx[0] = x
def on_mouse_y(y): my[0] = y
def on_left_click(state):
    if state:
        # stamp a circle where clicked
        wd.writecircle(mx[0], my[0], 8, 0xFF4400, fill=1)

def on_key(code):
    key = code & 0xFFFF
    if key == 70:   # F key — toggle fullscreen hint
        wd.writetext(10, 460, wd.FONT_8X8, 0x00FF88, "Press F11 for fullscreen")
    elif key == 67:  # C key — demo clipboard
        wd.setclipboard("Hello from MicroPython!")

wd.setcallbackfunc(wd.X_MOUSE,  on_mouse_x)
wd.setcallbackfunc(wd.Y_MOUSE,  on_mouse_y)
wd.setcallbackfunc(wd.L_CLICK,  on_left_click)
wd.setcallbackfunc(wd.KEY,      on_key)

# ── helpers ────────────────────────────────────────────────────
def hsv_to_rgb(h, s, v):
    h = h % 360
    c = v * s
    x = c * (1 - abs((h/60) % 2 - 1))
    m = v - c
    if   h < 60:  r,g,b = c,x,0
    elif h < 120: r,g,b = x,c,0
    elif h < 180: r,g,b = 0,c,x
    elif h < 240: r,g,b = 0,x,c
    elif h < 300: r,g,b = x,0,c
    else:         r,g,b = c,0,x
    return (int((r+m)*255)<<16)|(int((g+m)*255)<<8)|int((b+m)*255)

# ── draw static scene ──────────────────────────────────────────
def draw_scene(t):
    wd.clear(0x0a0a1a)

    # rainbow border
    for i in range(720):
        c = hsv_to_rgb(i/720*360, 1, 1)
        wd.writepixel(i, 0,   c)
        wd.writepixel(i, 479, c)
    for i in range(480):
        c = hsv_to_rgb(i/480*360, 1, 1)
        wd.writepixel(0,   i, c)
        wd.writepixel(719, i, c)

    # spinning triangle
    cx, cy, r = 360, 200, 80
    a = t * 0.05
    pts = [(int(cx + r*math.cos(a + i*2.094)),
            int(cy + r*math.sin(a + i*2.094))) for i in range(3)]
    wd.writetriangle(pts[0][0],pts[0][1], pts[1][0],pts[1][1],
                     pts[2][0],pts[2][1], hsv_to_rgb(t*3 % 360, 1, 1), fill=1)

    # concentric circles
    for i in range(5, 60, 10):
        wd.writecircle(160, 350, i, hsv_to_rgb(t*5+i*12 % 360, 0.8, 1))

    # filled boxes
    for i in range(8):
        wd.writebox(520+i*20, 300, 16, 80,
                    hsv_to_rgb((i*45 + t*2) % 360, 1, 1), fill=1)

    # diagonal lines
    for i in range(0, 100, 8):
        wd.writeline(0, i, i, 0, hsv_to_rgb(i*3 + t, 1, 0.9))

    # bouncing pixel ball
    bx = int(360 + 200*math.sin(t*0.07))
    by = int(350 + 60*math.sin(t*0.13))
    wd.writecircle(bx, by, 6, 0xFFFFFF, fill=1)

    # cursor crosshair
    wd.writeline(mx[0]-10, my[0], mx[0]+10, my[0], 0x00FF88)
    wd.writeline(mx[0], my[0]-10, mx[0], my[0]+10, 0x00FF88)
    wd.writecircle(mx[0], my[0], 4, 0x00FF88)

    # HUD text
    wd.writetext(10, 10, wd.FONT_8X8,  0x00FF88,
                 f"webdisplay  t={t:4d}")
    wd.writetext(10, 22, wd.FONT_8X8,  0x888888,
                 f"mouse {mx[0]:4d},{my[0]:4d}  btn={wd.getmouse(wd.L_CLICK)}")
    wd.writetext(10, 34, wd.FONT_16X16, 0xFFAA00, "Hello!")
    wd.writetext(10, 460, wd.FONT_8X8, 0x444466,
                 "Click=stamp  F=hint  C=clipboard  dblclick=fullscreen")

# ── main loop ─────────────────────────────────────────────────
print("[webdisplay] Open http://<device-ip>:6868 in your browser")
print("[webdisplay] Ctrl-C to stop")
t = 0
try:
    while True:
        wd.poll()
        draw_scene(t)
        t += 1
        time.sleep_ms(16)   # ~60 fps target
except KeyboardInterrupt:
    pass
