"""
webdisplay.py  —  Pure-Python MicroPython implementation
Drop this file onto your device alongside index.html (in /www/ or inline).
For platforms where building the C extension is impractical.

Usage:
    import webdisplay as wd
    wd.begin()
    while True:
        wd.poll()
        wd.clear(0x001122)
        wd.writepixel(100, 100, 0xFF0000)
        ...
"""

import socket, struct, hashlib, binascii, os

# ── constants (same as C version) ──────────────────────────────
X_MOUSE  = 0
Y_MOUSE  = 1
L_CLICK  = 2
R_CLICK  = 3
M_CLICK  = 4
KEY      = 5

FONT_8X8   = 0
FONT_16X16 = 1
FONT_MONO  = 2

_OP_PIXEL      = 0x01
_OP_LINE       = 0x02
_OP_BOX        = 0x03
_OP_CIRCLE     = 0x04
_OP_TRIANGLE   = 0x05
_OP_TEXT       = 0x06
_OP_BMP        = 0x07
_OP_CLEAR      = 0x08
_OP_RESOLUTION = 0x09
_OP_SET_CLIP   = 0x20

_EV_MOUSE     = 0x10
_EV_KEY       = 0x11
_EV_CLIPBOARD = 0x12

_WS_MAGIC = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# ── embedded HTML (loaded from same dir as this file) ──────────
def _load_html():
    p = __file__.replace('webdisplay.py', 'index.html')
    try:
        with open(p, 'rb') as f:
            return f.read()
    except OSError:
        return b"<h1>webdisplay: index.html not found</h1>"

_HTML = None  # lazy load

# ── state ───────────────────────────────────────────────────────
_server = None
_clients = []          # list of [sock, upgraded, rx_buf]
_width   = 640
_height  = 480
_mouse_x = 0
_mouse_y = 0
_mouse_buttons = 0
_last_key = 0
_clipboard = ""

_callbacks = {
    X_MOUSE:  None, Y_MOUSE:  None,
    L_CLICK:  None, R_CLICK:  None,
    M_CLICK:  None, KEY: None,
}

# ── WebSocket helpers ────────────────────────────────────────────
def _ws_accept_key(key):
    import ubinascii, uhashlib
    raw = key.encode() + _WS_MAGIC
    try:
        h = uhashlib.sha1(raw)
    except Exception:
        import hashlib
        h = hashlib.sha1(raw)
    try:
        return ubinascii.b2a_base64(h.digest()).decode().strip()
    except Exception:
        import base64
        return base64.b64encode(h.digest()).decode()

def _ws_handshake(sock, request):
    for line in request.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":",1)[1].strip().decode()
            accept = _ws_accept_key(key)
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            )
            sock.send(response.encode())
            return True
    return False

def _ws_send_binary(sock, data):
    """Send a binary WebSocket frame."""
    if isinstance(data, (list, bytearray)):
        data = bytes(data)
    l = len(data)
    if l <= 125:
        hdr = bytes([0x82, l])
    elif l < 65536:
        hdr = struct.pack(">BBH", 0x82, 126, l)
    else:
        hdr = struct.pack(">BBQ", 0x82, 127, l)
    try:
        sock.send(hdr + data)
        return True
    except OSError:
        return False

def _ws_recv_frame(sock):
    """Non-blocking receive; returns (payload_bytes) or None."""
    try:
        hdr = sock.recv(2)
        if not hdr or len(hdr) < 2:
            return None
    except OSError:
        return None
    opcode = hdr[0] & 0x0f
    if opcode == 8:      # close
        raise OSError("ws close")
    masked = bool(hdr[1] & 0x80)
    plen   = hdr[1] & 0x7f
    if plen == 126:
        ext = sock.recv(2)
        plen = struct.unpack(">H", ext)[0]
    elif plen == 127:
        ext = sock.recv(8)
        plen = struct.unpack(">Q", ext)[0]
    mask = sock.recv(4) if masked else b'\x00\x00\x00\x00'
    payload = bytearray(sock.recv(plen))
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
    return bytes(payload)

# ── broadcast ────────────────────────────────────────────────────
def _broadcast(data):
    global _clients
    if isinstance(data, (list, bytearray)):
        data = bytes(data)
    dead = []
    for i, (sock, upgraded, _) in enumerate(_clients):
        if upgraded:
            if not _ws_send_binary(sock, data):
                dead.append(i)
    for i in reversed(dead):
        try: _clients[i][0].close()
        except: pass
        _clients.pop(i)
    return len(_clients) > 0

def _p16(v): return struct.pack(">H", v & 0xffff)
def _p24(v): return bytes([(v>>16)&0xff, (v>>8)&0xff, v&0xff])

# ── public API ───────────────────────────────────────────────────
def begin(port=6868):
    global _server, _HTML
    _HTML = _load_html()
    _server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    _server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    _server.bind(('0.0.0.0', port))
    _server.listen(4)
    _server.setblocking(False)
    print(f"[webdisplay] listening on :{port}")
    return True

def poll():
    global _clients, _mouse_x, _mouse_y, _mouse_buttons, _last_key, _clipboard
    # accept new
    try:
        conn, addr = _server.accept()
        conn.setblocking(False)
        _clients.append([conn, False, b""])
    except OSError:
        pass

    dead = []
    for i, entry in enumerate(_clients):
        sock, upgraded, rbuf = entry
        # try read
        try:
            chunk = sock.recv(1024)
            if not chunk:
                dead.append(i); continue
            rbuf += chunk
            entry[2] = rbuf
        except OSError as e:
            # EAGAIN / EWOULDBLOCK — no data, that's fine
            import errno as _errno
            if hasattr(_errno, 'EAGAIN') and e.args[0] == _errno.EAGAIN:
                pass
            elif e.args[0] == 11:   # EAGAIN on MicroPython
                pass
            else:
                dead.append(i); continue

        if not upgraded:
            if b"\r\n\r\n" in rbuf:
                if b"Upgrade: websocket" in rbuf:
                    if _ws_handshake(sock, rbuf):
                        entry[1] = True
                        entry[2] = b""
                        # send resolution
                        _broadcast(bytes([_OP_RESOLUTION]) + _p16(_width) + _p16(_height))
                elif b"GET / " in rbuf or b"GET /index.html" in rbuf:
                    body = _HTML
                    hdr = (f"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                           f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode()
                    try:
                        sock.send(hdr + body)
                    except: pass
                    dead.append(i)
        else:
            # parse WS frames from rbuf is complex with non-blocking;
            # simpler: try recv directly (socket briefly blocking here is ok)
            try:
                frame = _ws_recv_frame(sock)
                if frame:
                    _handle_event(frame)
            except OSError:
                dead.append(i)

    for i in reversed(dead):
        try: _clients[i][0].close()
        except: pass
        _clients.pop(i)

def _handle_event(data):
    global _mouse_x, _mouse_y, _mouse_buttons, _last_key, _clipboard
    if not data: return
    t = data[0]
    if t == _EV_MOUSE and len(data) >= 6:
        nx = struct.unpack(">h", data[1:3])[0]
        ny = struct.unpack(">h", data[3:5])[0]
        nb = data[5]
        if nx != _mouse_x:
            _mouse_x = nx
            _fire(X_MOUSE, nx)
        if ny != _mouse_y:
            _mouse_y = ny
            _fire(Y_MOUSE, ny)
        if (nb & 1) != (_mouse_buttons & 1):
            _fire(L_CLICK, nb & 1)
        if (nb & 2) != (_mouse_buttons & 2):
            _fire(R_CLICK, (nb>>1) & 1)
        _mouse_buttons = nb
    elif t == _EV_KEY and len(data) >= 5:
        _last_key = struct.unpack(">I", data[1:5])[0]
        _fire(KEY, _last_key)
    elif t == _EV_CLIPBOARD and len(data) >= 2:
        _clipboard = data[1:].decode('utf-8', 'replace')
        if _callbacks.get(KEY):  # reuse KEY slot? No—clipboard doesn't have its own constant
            pass

def _fire(which, val):
    cb = _callbacks.get(which)
    if cb:
        cb(val)

# ── drawing ──────────────────────────────────────────────────────
def setresolution(w, h):
    global _width, _height
    _width, _height = w, h
    _broadcast(bytes([_OP_RESOLUTION]) + _p16(w) + _p16(h))

def writepixel(x, y, rgb):
    return _broadcast(bytes([_OP_PIXEL]) + _p16(x) + _p16(y) + _p24(rgb) + b'\x00')

def writeline(x0, y0, x1, y1, rgb):
    return _broadcast(bytes([_OP_LINE]) + _p16(x0)+_p16(y0)+_p16(x1)+_p16(y1) + _p24(rgb) + b'\x00')

def writebox(x, y, w, h, rgb, fill=0):
    return _broadcast(bytes([_OP_BOX]) + _p16(x)+_p16(y)+_p16(w)+_p16(h)
                      + _p24(rgb) + b'\x00' + bytes([fill]))

def writecircle(cx, cy, r, rgb, fill=0):
    return _broadcast(bytes([_OP_CIRCLE]) + _p16(cx)+_p16(cy)+_p16(r)
                      + _p24(rgb) + bytes([fill]))

def writetriangle(x0,y0,x1,y1,x2,y2, rgb, fill=0):
    return _broadcast(bytes([_OP_TRIANGLE])
                      + _p16(x0)+_p16(y0)+_p16(x1)+_p16(y1)+_p16(x2)+_p16(y2)
                      + _p24(rgb) + bytes([fill]))

def writetext(x, y, font, rgb, text):
    enc = text.encode('utf-8')
    return _broadcast(bytes([_OP_TEXT]) + _p16(x)+_p16(y)
                      + bytes([font]) + _p24(rgb) + b'\x00' + enc)

def writebmp(x, y, w, h, data):
    if hasattr(data, 'tobytes'): data = data.tobytes()
    return _broadcast(bytes([_OP_BMP]) + _p16(x)+_p16(y)+_p16(w)+_p16(h) + bytes(data))

def clear(rgb=0):
    return _broadcast(bytes([_OP_CLEAR]) + _p24(rgb) + b'\x00')

# ── input ────────────────────────────────────────────────────────
def getmouse(which):
    if which == X_MOUSE:  return _mouse_x
    if which == Y_MOUSE:  return _mouse_y
    if which == L_CLICK:  return (_mouse_buttons >> 0) & 1
    if which == R_CLICK:  return (_mouse_buttons >> 1) & 1
    if which == M_CLICK:  return (_mouse_buttons >> 2) & 1
    if which == KEY:      return _last_key
    return None

def getclipboard():
    return _clipboard

def setclipboard(text):
    enc = text.encode('utf-8')
    return _broadcast(bytes([_OP_SET_CLIP]) + enc)

def setcallbackfunc(which, fn):
    _callbacks[which] = fn
