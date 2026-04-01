/*
 * webdisplay.c — MicroPython C extension
 * Serves a fullscreenable canvas over HTTP/WebSocket on port 6868.
 * Implements a binary frame-buffer protocol for speed.
 *
 * Build: see CMakeLists.txt or Makefile.
 * Requires: lwIP sockets (ESP32/RP2) or POSIX sockets (unix port).
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "py/objstr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ── platform socket shim ────────────────────────────────────────── */
#if defined(__unix__) || defined(__APPLE__)
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  #define SOCK_NONBLOCK_SET(fd) fcntl(fd, F_SETFL, O_NONBLOCK)
  #define CLOSESOCK(fd)         close(fd)
#else
  /* ESP32 / lwIP */
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #define SOCK_NONBLOCK_SET(fd) \
      do { int f=1; ioctlsocket(fd,FIONBIO,&f); } while(0)
  #define CLOSESOCK(fd)         close(fd)
#endif

/* ── constants ───────────────────────────────────────────────────── */
#define WD_PORT       6868
#define WD_MAX_CONN   4
#define WD_FB_MAX_W   1920
#define WD_FB_MAX_H   1080
#define WD_BUF_SIZE   4096

/* binary opcodes sent to browser */
#define OP_PIXEL      0x01
#define OP_LINE       0x02
#define OP_BOX        0x03
#define OP_CIRCLE     0x04
#define OP_TRIANGLE   0x05
#define OP_TEXT       0x06
#define OP_BMP        0x07
#define OP_CLEAR      0x08
#define OP_RESOLUTION 0x09

/* binary opcodes received from browser */
#define EV_MOUSE      0x10
#define EV_KEY        0x11
#define EV_CLIPBOARD  0x12

/* ── state ───────────────────────────────────────────────────────── */
typedef struct {
    int  server_fd;
    int  client_fd[WD_MAX_CONN];
    int  client_count;
    int  width, height;

    /* input state */
    int16_t  mouse_x, mouse_y;
    uint8_t  mouse_buttons;   /* bit0=left, bit1=right, bit2=middle */
    uint32_t last_key;
    char     clipboard[1024];

    /* callbacks: mp_obj_t or MP_OBJ_NULL */
    mp_obj_t cb_mouse_x;
    mp_obj_t cb_mouse_y;
    mp_obj_t cb_left_click;
    mp_obj_t cb_right_click;
    mp_obj_t cb_key;
    mp_obj_t cb_clipboard;

    uint8_t  ws_upgraded[WD_MAX_CONN];  /* 1 if handshake done */
    uint8_t  tx_buf[WD_BUF_SIZE];
    uint8_t  rx_buf[WD_BUF_SIZE];
} wd_state_t;

static wd_state_t wd = {
    .server_fd    = -1,
    .client_count = 0,
    .width  = 640,
    .height = 480,
    .cb_mouse_x    = MP_OBJ_NULL,
    .cb_mouse_y    = MP_OBJ_NULL,
    .cb_left_click = MP_OBJ_NULL,
    .cb_right_click= MP_OBJ_NULL,
    .cb_key        = MP_OBJ_NULL,
    .cb_clipboard  = MP_OBJ_NULL,
};

/* ── embedded HTML ───────────────────────────────────────────────── */
/* Generated at build time from html/index.html by xxd -i; see Makefile */
#include "webdisplay_html.h"   /* declares: extern const uint8_t wd_html[]; extern size_t wd_html_len; */

/* ── WebSocket helpers ───────────────────────────────────────────── */
#include "webdisplay_ws.h"     /* ws_handshake(), ws_send_binary(), ws_recv_frame() */

/* ── send a raw binary frame to all connected WS clients ─────────── */
static int wd_broadcast(const uint8_t *data, size_t len) {
    int sent = 0;
    for (int i = 0; i < WD_MAX_CONN; i++) {
        if (wd.client_fd[i] >= 0 && wd.ws_upgraded[i]) {
            if (ws_send_binary(wd.client_fd[i], data, len) == 0) sent++;
        }
    }
    return sent > 0 ? 0 : -1;
}

/* pack uint16_t big-endian */
static inline void put16(uint8_t *b, uint16_t v) {
    b[0] = v >> 8; b[1] = v & 0xff;
}
static inline void put32(uint8_t *b, uint32_t v) {
    b[0]=v>>24; b[1]=(v>>16)&0xff; b[2]=(v>>8)&0xff; b[3]=v&0xff;
}

/* ── accept + poll (call from poll()) ───────────────────────────── */
static void wd_accept_new(void) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(wd.server_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) return;
    SOCK_NONBLOCK_SET(fd);
    for (int i = 0; i < WD_MAX_CONN; i++) {
        if (wd.client_fd[i] < 0) {
            wd.client_fd[i] = fd;
            wd.ws_upgraded[i] = 0;
            wd.client_count++;
            return;
        }
    }
    CLOSESOCK(fd); /* no room */
}

static void wd_fire_callback(mp_obj_t cb, mp_obj_t arg) {
    if (cb != MP_OBJ_NULL && cb != mp_const_none) {
        mp_call_function_1(cb, arg);
    }
}

static void wd_process_event(uint8_t *data, size_t len) {
    if (len < 1) return;
    uint8_t type = data[0];
    if (type == EV_MOUSE && len >= 7) {
        int16_t nx = (int16_t)((data[1]<<8)|data[2]);
        int16_t ny = (int16_t)((data[3]<<8)|data[4]);
        uint8_t nb = data[5];
        if (nx != wd.mouse_x) {
            wd.mouse_x = nx;
            wd_fire_callback(wd.cb_mouse_x, MP_OBJ_NEW_SMALL_INT(nx));
        }
        if (ny != wd.mouse_y) {
            wd.mouse_y = ny;
            wd_fire_callback(wd.cb_mouse_y, MP_OBJ_NEW_SMALL_INT(ny));
        }
        if ((nb & 1) != (wd.mouse_buttons & 1))
            wd_fire_callback(wd.cb_left_click, MP_OBJ_NEW_SMALL_INT(nb & 1));
        if ((nb & 2) != (wd.mouse_buttons & 2))
            wd_fire_callback(wd.cb_right_click, MP_OBJ_NEW_SMALL_INT((nb>>1)&1));
        wd.mouse_buttons = nb;
    } else if (type == EV_KEY && len >= 5) {
        uint32_t key = ((uint32_t)data[1]<<24)|((uint32_t)data[2]<<16)|
                       ((uint32_t)data[3]<<8)|data[4];
        wd.last_key = key;
        wd_fire_callback(wd.cb_key, MP_OBJ_NEW_SMALL_INT((mp_int_t)key));
    } else if (type == EV_CLIPBOARD && len >= 2) {
        size_t slen = len - 1;
        if (slen >= sizeof(wd.clipboard)) slen = sizeof(wd.clipboard)-1;
        memcpy(wd.clipboard, data+1, slen);
        wd.clipboard[slen] = 0;
        wd_fire_callback(wd.cb_clipboard,
            mp_obj_new_str(wd.clipboard, slen));
    }
}

static void wd_poll_client(int idx) {
    int fd = wd.client_fd[idx];
    if (!wd.ws_upgraded[idx]) {
        /* try HTTP upgrade */
        char hbuf[1024];
        int n = recv(fd, hbuf, sizeof(hbuf)-1, 0);
        if (n <= 0) { CLOSESOCK(fd); wd.client_fd[idx]=-1; wd.client_count--; return; }
        hbuf[n] = 0;
        if (strstr(hbuf, "Upgrade: websocket")) {
            ws_handshake(fd, hbuf);
            wd.ws_upgraded[idx] = 1;
            /* send current resolution */
            uint8_t msg[5] = {OP_RESOLUTION};
            put16(msg+1, (uint16_t)wd.width);
            put16(msg+3, (uint16_t)wd.height);
            ws_send_binary(fd, msg, 5);
        } else if (strstr(hbuf, "GET / ") || strstr(hbuf, "GET /index.html")) {
            /* serve HTML */
            char hdr[128];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                wd_html_len);
            send(fd, hdr, hlen, 0);
            send(fd, wd_html, wd_html_len, 0);
            CLOSESOCK(fd); wd.client_fd[idx]=-1; wd.client_count--;
        }
        return;
    }
    /* WS frame */
    uint8_t *payload; size_t plen;
    int r = ws_recv_frame(fd, wd.rx_buf, WD_BUF_SIZE, &payload, &plen);
    if (r < 0) { CLOSESOCK(fd); wd.client_fd[idx]=-1; wd.client_count--; return; }
    if (r > 0) wd_process_event(payload, plen);
}

/* ── MicroPython API ─────────────────────────────────────────────── */

STATIC mp_obj_t wd_begin(void) {
    if (wd.server_fd >= 0) return mp_const_true;
    for (int i=0;i<WD_MAX_CONN;i++) wd.client_fd[i]=-1;

    wd.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (wd.server_fd < 0) return mp_const_false;
    int yes=1; setsockopt(wd.server_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    SOCK_NONBLOCK_SET(wd.server_fd);

    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(WD_PORT);
    addr.sin_addr.s_addr=INADDR_ANY;
    if (bind(wd.server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
        CLOSESOCK(wd.server_fd); wd.server_fd=-1; return mp_const_false;
    }
    listen(wd.server_fd, WD_MAX_CONN);
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wd_begin_obj, wd_begin);

STATIC mp_obj_t wd_poll(void) {
    if (wd.server_fd < 0) return mp_const_none;
    wd_accept_new();
    for (int i=0;i<WD_MAX_CONN;i++)
        if (wd.client_fd[i]>=0) wd_poll_client(i);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wd_poll_obj, wd_poll);

STATIC mp_obj_t wd_setresolution(mp_obj_t w_o, mp_obj_t h_o) {
    wd.width  = mp_obj_get_int(w_o);
    wd.height = mp_obj_get_int(h_o);
    uint8_t msg[5] = {OP_RESOLUTION};
    put16(msg+1,(uint16_t)wd.width);
    put16(msg+3,(uint16_t)wd.height);
    wd_broadcast(msg,5);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(wd_setresolution_obj, wd_setresolution);

/* writepixel(x, y, rgb) */
STATIC mp_obj_t wd_writepixel(mp_obj_t x_o, mp_obj_t y_o, mp_obj_t rgb_o) {
    uint8_t b[8] = {OP_PIXEL};
    put16(b+1,(uint16_t)mp_obj_get_int(x_o));
    put16(b+3,(uint16_t)mp_obj_get_int(y_o));
    put32(b+4,(uint32_t)mp_obj_get_int(rgb_o));
    /* only 3 bytes of colour needed, but 4 keeps alignment; browser ignores alpha */
    return wd_broadcast(b,8)==0 ? mp_const_none : mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(wd_writepixel_obj, wd_writepixel);

/* writeline(x0,y0,x1,y1,rgb) */
STATIC mp_obj_t wd_writeline(size_t n, const mp_obj_t *a) {
    if (n<5) mp_raise_ValueError(MP_ERROR_TEXT("writeline: 5 args"));
    uint8_t b[12]={OP_LINE};
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    put16(b+5,(uint16_t)mp_obj_get_int(a[2]));
    put16(b+7,(uint16_t)mp_obj_get_int(a[3]));
    put32(b+8,(uint32_t)mp_obj_get_int(a[4])); /* but only 3 used */
    return wd_broadcast(b,12)==0 ? mp_const_none : mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writeline_obj, 5, 5, wd_writeline);

/* writebox(x,y,w,h,rgb,fill=0) */
STATIC mp_obj_t wd_writebox(size_t n, const mp_obj_t *a) {
    if (n<5) mp_raise_ValueError(MP_ERROR_TEXT("writebox: 5 args"));
    uint8_t b[14]={OP_BOX};
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    put16(b+5,(uint16_t)mp_obj_get_int(a[2]));
    put16(b+7,(uint16_t)mp_obj_get_int(a[3]));
    put32(b+9,(uint32_t)mp_obj_get_int(a[4]));
    b[13]=(n>=6)?(uint8_t)mp_obj_get_int(a[5]):0;
    return wd_broadcast(b,14)==0?mp_const_none:mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writebox_obj, 5, 6, wd_writebox);

/* writecircle(cx,cy,r,rgb,fill=0) */
STATIC mp_obj_t wd_writecircle(size_t n, const mp_obj_t *a) {
    if (n<4) mp_raise_ValueError(MP_ERROR_TEXT("writecircle: 4 args"));
    uint8_t b[12]={OP_CIRCLE};
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    put16(b+5,(uint16_t)mp_obj_get_int(a[2]));
    put32(b+7,(uint32_t)mp_obj_get_int(a[3]));
    b[11]=(n>=5)?(uint8_t)mp_obj_get_int(a[4]):0;
    return wd_broadcast(b,12)==0?mp_const_none:mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writecircle_obj, 4, 5, wd_writecircle);

/* writetriangle(x0,y0,x1,y1,x2,y2,rgb,fill=0) */
STATIC mp_obj_t wd_writetriangle(size_t n, const mp_obj_t *a) {
    if (n<7) mp_raise_ValueError(MP_ERROR_TEXT("writetriangle: 7 args"));
    uint8_t b[16]={OP_TRIANGLE};
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    put16(b+5,(uint16_t)mp_obj_get_int(a[2]));
    put16(b+7,(uint16_t)mp_obj_get_int(a[3]));
    put16(b+9,(uint16_t)mp_obj_get_int(a[4]));
    put16(b+11,(uint16_t)mp_obj_get_int(a[5]));
    put32(b+12,(uint32_t)mp_obj_get_int(a[6])); /* only 3 used */
    b[15]=(n>=8)?(uint8_t)mp_obj_get_int(a[7]):0; /* fill (reuse byte 15, harmless) */
    /* note: we send 16 bytes; rgb occupies b[12..14], fill b[15] — perfect */
    return wd_broadcast(b,16)==0?mp_const_none:mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writetriangle_obj, 7, 8, wd_writetriangle);

/* writetext(x, y, font, rgb, text) — font: 0=builtin 8x8, 1=builtin 16x16 */
STATIC mp_obj_t wd_writetext(size_t n, const mp_obj_t *a) {
    if (n<5) mp_raise_ValueError(MP_ERROR_TEXT("writetext: 5 args"));
    size_t tlen;
    const char *txt = mp_obj_str_get_data(a[4], &tlen);
    size_t total = 1+2+2+1+4+tlen;
    uint8_t *b = (uint8_t*)m_malloc(total);
    b[0]=OP_TEXT;
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    b[5]=(uint8_t)mp_obj_get_int(a[2]);
    put32(b+6,(uint32_t)mp_obj_get_int(a[3]));
    memcpy(b+10, txt, tlen);
    int r = wd_broadcast(b, total);
    m_free(b);
    return r==0?mp_const_none:mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writetext_obj, 5, 5, wd_writetext);

/* writebmp(x, y, bytes_data) — raw RGB888 bytes, width/height inferred from buffer length */
STATIC mp_obj_t wd_writebmp(size_t n, const mp_obj_t *a) {
    if (n<5) mp_raise_ValueError(MP_ERROR_TEXT("writebmp: x,y,w,h,data"));
    mp_buffer_info_t bi;
    mp_get_buffer_raise(a[4], &bi, MP_BUFFER_READ);
    size_t total = 1+2+2+2+2+bi.len;
    uint8_t *b = (uint8_t*)m_malloc(total);
    b[0]=OP_BMP;
    put16(b+1,(uint16_t)mp_obj_get_int(a[0]));
    put16(b+3,(uint16_t)mp_obj_get_int(a[1]));
    put16(b+5,(uint16_t)mp_obj_get_int(a[2]));
    put16(b+7,(uint16_t)mp_obj_get_int(a[3]));
    memcpy(b+9, bi.buf, bi.len);
    int r = wd_broadcast(b, total);
    m_free(b);
    return r==0?mp_const_none:mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_writebmp_obj, 5, 5, wd_writebmp);

/* clear(rgb=0x000000) */
STATIC mp_obj_t wd_clear(size_t n, const mp_obj_t *a) {
    uint8_t b[5]={OP_CLEAR};
    uint32_t col = (n>=1)?((uint32_t)mp_obj_get_int(a[0])):0;
    put32(b+1,col);
    wd_broadcast(b,5);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wd_clear_obj, 0, 1, wd_clear);

/* ── input getters ───────────────────────────────────────────────── */
#define WD_CONST_MOUSE_X    0
#define WD_CONST_MOUSE_Y    1
#define WD_CONST_L_CLICK    2
#define WD_CONST_R_CLICK    3
#define WD_CONST_M_CLICK    4
#define WD_CONST_KEY        5

STATIC mp_obj_t wd_getmouse(mp_obj_t which_o) {
    int which = mp_obj_get_int(which_o);
    switch(which) {
        case WD_CONST_MOUSE_X: return MP_OBJ_NEW_SMALL_INT(wd.mouse_x);
        case WD_CONST_MOUSE_Y: return MP_OBJ_NEW_SMALL_INT(wd.mouse_y);
        case WD_CONST_L_CLICK: return MP_OBJ_NEW_SMALL_INT(wd.mouse_buttons & 1);
        case WD_CONST_R_CLICK: return MP_OBJ_NEW_SMALL_INT((wd.mouse_buttons>>1)&1);
        case WD_CONST_M_CLICK: return MP_OBJ_NEW_SMALL_INT((wd.mouse_buttons>>2)&1);
        case WD_CONST_KEY:     return MP_OBJ_NEW_SMALL_INT((mp_int_t)wd.last_key);
        default: return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(wd_getmouse_obj, wd_getmouse);

STATIC mp_obj_t wd_getclipboard(void) {
    return mp_obj_new_str(wd.clipboard, strlen(wd.clipboard));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wd_getclipboard_obj, wd_getclipboard);

/* setclipboard(str) — sends a special WS message asking browser to write clipboard */
STATIC mp_obj_t wd_setclipboard(mp_obj_t s_o) {
    size_t len; const char *s = mp_obj_str_get_data(s_o, &len);
    uint8_t *b = (uint8_t*)m_malloc(1+len);
    b[0] = 0x20; /* OP_SET_CLIPBOARD */
    memcpy(b+1, s, len);
    wd_broadcast(b, 1+len);
    m_free(b);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(wd_setclipboard_obj, wd_setclipboard);

/* setcallbackfunc(which, func) */
STATIC mp_obj_t wd_setcallbackfunc(mp_obj_t which_o, mp_obj_t fn_o) {
    int which = mp_obj_get_int(which_o);
    switch(which) {
        case WD_CONST_MOUSE_X: wd.cb_mouse_x    = fn_o; break;
        case WD_CONST_MOUSE_Y: wd.cb_mouse_y    = fn_o; break;
        case WD_CONST_L_CLICK: wd.cb_left_click = fn_o; break;
        case WD_CONST_R_CLICK: wd.cb_right_click= fn_o; break;
        case WD_CONST_KEY:     wd.cb_key        = fn_o; break;
        default: break;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(wd_setcallbackfunc_obj, wd_setcallbackfunc);

/* ── module table ────────────────────────────────────────────────── */
STATIC const mp_rom_map_elem_t wd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR_webdisplay) },

    /* lifecycle */
    { MP_ROM_QSTR(MP_QSTR_begin),           MP_ROM_PTR(&wd_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll),            MP_ROM_PTR(&wd_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_setresolution),   MP_ROM_PTR(&wd_setresolution_obj) },

    /* drawing */
    { MP_ROM_QSTR(MP_QSTR_writepixel),      MP_ROM_PTR(&wd_writepixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeline),       MP_ROM_PTR(&wd_writeline_obj) },
    { MP_ROM_QSTR(MP_QSTR_writebox),        MP_ROM_PTR(&wd_writebox_obj) },
    { MP_ROM_QSTR(MP_QSTR_writecircle),     MP_ROM_PTR(&wd_writecircle_obj) },
    { MP_ROM_QSTR(MP_QSTR_writetriangle),   MP_ROM_PTR(&wd_writetriangle_obj) },
    { MP_ROM_QSTR(MP_QSTR_writetext),       MP_ROM_PTR(&wd_writetext_obj) },
    { MP_ROM_QSTR(MP_QSTR_writebmp),        MP_ROM_PTR(&wd_writebmp_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),           MP_ROM_PTR(&wd_clear_obj) },

    /* input */
    { MP_ROM_QSTR(MP_QSTR_getmouse),        MP_ROM_PTR(&wd_getmouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_getclipboard),    MP_ROM_PTR(&wd_getclipboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_setclipboard),    MP_ROM_PTR(&wd_setclipboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_setcallbackfunc), MP_ROM_PTR(&wd_setcallbackfunc_obj) },

    /* input constants */
    { MP_ROM_QSTR(MP_QSTR_X_MOUSE),  MP_ROM_INT(WD_CONST_MOUSE_X) },
    { MP_ROM_QSTR(MP_QSTR_Y_MOUSE),  MP_ROM_INT(WD_CONST_MOUSE_Y) },
    { MP_ROM_QSTR(MP_QSTR_L_CLICK),  MP_ROM_INT(WD_CONST_L_CLICK) },
    { MP_ROM_QSTR(MP_QSTR_R_CLICK),  MP_ROM_INT(WD_CONST_R_CLICK) },
    { MP_ROM_QSTR(MP_QSTR_M_CLICK),  MP_ROM_INT(WD_CONST_M_CLICK) },
    { MP_ROM_QSTR(MP_QSTR_KEY),      MP_ROM_INT(WD_CONST_KEY) },

    /* font constants */
    { MP_ROM_QSTR(MP_QSTR_FONT_8X8),   MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_FONT_16X16), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_FONT_MONO),  MP_ROM_INT(2) },
};
STATIC MP_DEFINE_CONST_DICT(wd_module_globals, wd_module_globals_table);

const mp_obj_module_t webdisplay_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&wd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webdisplay, webdisplay_module);
