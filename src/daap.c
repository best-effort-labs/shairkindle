#include "daap.h"
#include <string.h>
#define DAAP_MAX_DEPTH 8

static void take(char *dst, size_t cap, const uint8_t *v, uint32_t n){
    if (n > cap-1) n = (uint32_t)(cap-1);
    memcpy(dst, v, n); dst[n] = 0;   /* Raw truncation may clip a UTF-8 codepoint; display-only. */
}

/* returns 1 if [p,end) plausibly begins a nested DMAP item run: a printable
   4-char code followed by a length that fits. Bound-checks by SUBTRACTION
   (never forms an out-of-range pointer — that would be UB for a hostile n). */
static int looks_nested(const uint8_t *p, const uint8_t *end){
    if (end - p < 8) return 0;
    for (int i=0;i<4;i++){ uint8_t c=p[i]; if (c<0x20||c>0x7e) return 0; }
    uint32_t n = (uint32_t)p[4]<<24|(uint32_t)p[5]<<16|(uint32_t)p[6]<<8|p[7];
    return n <= (size_t)(end - (p + 8));   /* inner item's payload fits in [p+8,end) */
}

static void walk(const uint8_t *buf, size_t len, daap_meta_t *out, int depth){
    if (depth > DAAP_MAX_DEPTH) return;
    const uint8_t *p = buf, *end = buf + len;
    while (end - p >= 8){
        const uint8_t *code = p;
        uint32_t n = (uint32_t)p[4]<<24|(uint32_t)p[5]<<16|(uint32_t)p[6]<<8|p[7];
        const uint8_t *val = p + 8;
        if ((size_t)(end - val) < n) break;                 /* bounds: truncated length */
        if      (!memcmp(code,"minm",4)) take(out->title ,sizeof out->title ,val,n);
        else if (!memcmp(code,"asar",4)) take(out->artist,sizeof out->artist,val,n);
        else if (!memcmp(code,"asal",4)) take(out->album ,sizeof out->album ,val,n);
        else if (n >= 8 && looks_nested(val, val+n)) walk(val, n, out, depth+1);
        p = val + n;
    }
}

int daap_parse(const uint8_t *buf, size_t len, daap_meta_t *out){
    memset(out, 0, sizeof *out);
    if (buf && len >= 8) walk(buf, len, out, 0);
    return 0;
}
