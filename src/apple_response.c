#include "raop_crypto.h"
#include <string.h>
#include "bearssl_rsa.h"
#include "airport_key.h"
/* minimal base64 (no padding) */
static size_t b64_nopad(const uint8_t *in, size_t n, char *out) {
    static const char T[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0; size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = (in[i]<<16)|(in[i+1]<<8)|in[i+2];
        out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63];
        out[o++]=T[(v>>6)&63];  out[o++]=T[v&63]; i+=3;
    }
    if (n - i == 1) { uint32_t v=in[i]<<16; out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63]; }
    else if (n - i == 2) { uint32_t v=(in[i]<<16)|(in[i+1]<<8);
        out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63]; out[o++]=T[(v>>6)&63]; }
    out[o]=0; return o;
}
int raop_apple_response(const uint8_t *challenge, size_t clen,
                        const uint8_t ip4[4], const uint8_t mac[6],
                        char *out_b64, size_t out_cap) {
    if (!challenge || !ip4 || !mac || !out_b64 || clen == 0) return -1;
    size_t klen = (AIRPORT_RSA_SK.n_bitlen + 7) / 8;
    uint8_t blk[512];
    if (klen > sizeof blk || clen > 16) return -1;
    /* payload = challenge ‖ ip4 ‖ mac, zero-padded to 32 bytes */
    uint8_t payload[32]; memset(payload, 0, sizeof payload);
    size_t p = 0;
    memcpy(payload + p, challenge, clen); p += clen;
    memcpy(payload + p, ip4, 4);          p += 4;
    memcpy(payload + p, mac, 6);          p += 6;
    /* PKCS#1 type-1 pad: 0x00 0x01 0xFF..0xFF 0x00 <payload(32)> */
    size_t plen = 32;
    if (klen < plen + 11) return -1;
    size_t o = 0;
    blk[o++] = 0x00; blk[o++] = 0x01;
    size_t nff = klen - plen - 3;
    memset(blk + o, 0xFF, nff); o += nff;
    blk[o++] = 0x00;
    memcpy(blk + o, payload, plen); o += plen;   /* o == klen */
    if (!br_rsa_i31_private(blk, &AIRPORT_RSA_SK)) return -1;
    size_t need = (klen / 3) * 4 + (klen % 3 == 1 ? 2 : klen % 3 == 2 ? 3 : 0);
    if (out_cap < need + 1) return -1;
    return (int)b64_nopad(blk, klen, out_b64);
}
