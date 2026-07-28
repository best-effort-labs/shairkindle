/* src/b64.c — hand-rolled base64 decoder (no dependencies). */
#include "b64.h"

static int val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t n = 0; /* count of significant (non-padding, non-whitespace) chars */
    int seen_pad = 0; /* once '=' appears, only more '=' / whitespace may follow */

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') { seen_pad = 1; continue; }
        if (seen_pad) return -1; /* data char after padding started */
        int v = val(c);
        if (v < 0) return -1; /* invalid char */
        n++;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return -1; /* overflow */
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    /* A trailing group of exactly 1 base64 char can never be valid: the
     * minimum group that encodes any byte is 2 chars. */
    if (n % 4 == 1) return -1; /* bad length */

    *out_len = o;
    return 0;
}
