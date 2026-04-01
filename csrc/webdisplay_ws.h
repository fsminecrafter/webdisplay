/*
 * webdisplay_ws.h — tiny RFC-6455 WebSocket helpers (header-only)
 * Only the pieces needed: server handshake, binary send, masked recv.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── SHA-1 (minimal, for WS handshake) ──────────────────────────── */
typedef struct { uint32_t state[5]; uint64_t count; uint8_t buf[64]; } SHA1_CTX;

static void _sha1_transform(uint32_t s[5], const uint8_t b[64]) {
    uint32_t a,bb,c,d,e,t,w[80]; int i;
#define ROL(v,n) (((v)<<(n))|((v)>>(32-(n))))
    for(i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(;i<80;i++) w[i]=ROL(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=s[0];bb=s[1];c=s[2];d=s[3];e=s[4];
    for(i=0;i<80;i++){
        if(i<20)      t=ROL(a,5)+((bb&c)|(~bb&d))+e+w[i]+0x5A827999u;
        else if(i<40) t=ROL(a,5)+(bb^c^d)        +e+w[i]+0x6ED9EBA1u;
        else if(i<60) t=ROL(a,5)+((bb&c)|(bb&d)|(c&d))+e+w[i]+0x8F1BBCDCu;
        else          t=ROL(a,5)+(bb^c^d)        +e+w[i]+0xCA62C1D6u;
        e=d;d=c;c=ROL(bb,30);bb=a;a=t;
    }
    s[0]+=a;s[1]+=bb;s[2]+=c;s[3]+=d;s[4]+=e;
#undef ROL
}
static void sha1_init(SHA1_CTX *c){
    c->state[0]=0x67452301;c->state[1]=0xEFCDAB89;
    c->state[2]=0x98BADCFE;c->state[3]=0x10325476;c->state[4]=0xC3D2E1F0;
    c->count=0;
}
static void sha1_update(SHA1_CTX *c,const uint8_t *d,size_t l){
    size_t j=(c->count/8)%64;
    c->count+=(uint64_t)l*8;
    size_t i=0;
    if(j&&j+l>=64){memcpy(c->buf+j,d,64-j);_sha1_transform(c->state,c->buf);i=64-j;j=0;}
    for(;i+63<l;i+=64) _sha1_transform(c->state,d+i);
    memcpy(c->buf+j,d+i,l-i);
}
static void sha1_final(SHA1_CTX *c,uint8_t out[20]){
    uint8_t pad[8]; uint64_t cnt=c->count;
    uint8_t f=0x80; sha1_update(c,&f,1);
    while((c->count/8)%64!=56){f=0;sha1_update(c,&f,1);}
    for(int i=7;i>=0;i--){pad[i]=cnt&0xff;cnt>>=8;}
    sha1_update(c,pad,8);
    for(int i=0;i<5;i++){out[i*4]=(c->state[i]>>24)&0xff;out[i*4+1]=(c->state[i]>>16)&0xff;out[i*4+2]=(c->state[i]>>8)&0xff;out[i*4+3]=c->state[i]&0xff;}
}

/* base64 encode */
static const char _b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t *in,size_t ilen,char *out){
    size_t o=0;
    for(size_t i=0;i<ilen;i+=3){
        uint32_t v=((uint32_t)in[i]<<16)|(i+1<ilen?(uint32_t)in[i+1]<<8:0)|(i+2<ilen?in[i+2]:0);
        out[o++]=_b64[(v>>18)&63];out[o++]=_b64[(v>>12)&63];
        out[o++]=i+1<ilen?_b64[(v>>6)&63]:'=';
        out[o++]=i+2<ilen?_b64[v&63]:'=';
    }
    out[o]=0; return o;
}

/* WebSocket handshake */
static int ws_handshake(int fd, const char *req) {
    /* find Sec-WebSocket-Key */
    const char *p = strstr(req,"Sec-WebSocket-Key:");
    if(!p) return -1;
    p+=18; while(*p==' ')p++;
    char key[64]={0}; int i=0;
    while(*p && *p!='\r' && *p!='\n' && i<(int)sizeof(key)-1) key[i++]=*p++;
    key[i]=0;
    /* concat magic */
    char cat[128]; snprintf(cat,sizeof(cat),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    /* SHA1 + base64 */
    SHA1_CTX ctx; sha1_init(&ctx);
    sha1_update(&ctx,(uint8_t*)cat,strlen(cat));
    uint8_t digest[20]; sha1_final(&ctx,digest);
    char accept[32]; b64_encode(digest,20,accept);
    /* respond */
    char resp[256];
    int rlen=snprintf(resp,sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send(fd,resp,rlen,0);
    return 0;
}

/* Send binary WS frame */
static int ws_send_binary(int fd, const uint8_t *data, size_t len) {
    uint8_t hdr[10]; int hlen;
    hdr[0]=0x82; /* FIN + binary */
    if(len<=125)      { hdr[1]=(uint8_t)len; hlen=2; }
    else if(len<65536){ hdr[1]=126; hdr[2]=len>>8; hdr[3]=len&0xff; hlen=4; }
    else              { hdr[1]=127;
        hdr[2]=hdr[3]=hdr[4]=hdr[5]=0;
        hdr[6]=(len>>24)&0xff;hdr[7]=(len>>16)&0xff;
        hdr[8]=(len>>8)&0xff;hdr[9]=len&0xff; hlen=10; }
    if(send(fd,hdr,hlen,0)<0) return -1;
    if(send(fd,data,len,0)<0) return -1;
    return 0;
}

/* Receive one WS frame (non-blocking; returns 0=nothing, 1=frame, -1=error)
 * payload/plen set on return 1 */
static int ws_recv_frame(int fd, uint8_t *buf, size_t buflen,
                         uint8_t **payload_out, size_t *plen_out) {
    /* peek header */
    uint8_t hdr[2];
    int n = recv(fd, hdr, 2, MSG_DONTWAIT);
    if(n==0) return -1;
    if(n<0) return 0; /* EAGAIN */
    uint8_t masked = hdr[1]&0x80;
    size_t plen    = hdr[1]&0x7f;
    if(plen==126){ uint8_t x[2]; recv(fd,x,2,0); plen=((size_t)x[0]<<8)|x[1]; }
    else if(plen==127){ uint8_t x[8]; recv(fd,x,8,0);
        plen=0; for(int i=4;i<8;i++) plen=(plen<<8)|x[i]; }
    uint8_t mask[4]={0};
    if(masked) recv(fd,mask,4,0);
    if(plen>buflen) { /* drain */ while(plen>0){recv(fd,buf,plen<buflen?plen:buflen,0);plen=0;} return 0; }
    int got=0;
    while((size_t)got<plen){ int r=recv(fd,buf+got,plen-got,0); if(r<=0)break; got+=r; }
    if(masked) for(size_t i=0;i<plen;i++) buf[i]^=mask[i%4];
    /* opcode 8 = close */
    if((hdr[0]&0x0f)==8) return -1;
    *payload_out = buf;
    *plen_out    = plen;
    return 1;
}
