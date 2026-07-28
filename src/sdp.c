/* src/sdp.c — RAOP ANNOUNCE (SDP) body parse + validate + ALAC cookie build.
 *
 * No sockets, no allocation beyond the caller-supplied raop_sdp_t. All
 * multi-byte fields are assembled byte-by-byte (never a pointer cast to a
 * wider integer type) so this is safe on unaligned-fault targets (ARMv6).
 */
#include "sdp.h"
#include "b64.h"
#include <limits.h>
#include <string.h>

/* Find the next line in body[0..body_len) starting at *p (updated past the
 * line on return). Sets *line and *line_len to the line content with any
 * trailing CRLF or LF stripped. Returns 0 on success, -1 at end of input. */
static int next_line(const char *body, size_t body_len, size_t *p,
                      const char **line, size_t *line_len) {
    if (*p >= body_len) return -1;
    const char *start = body + *p;
    const char *end = body + body_len;
    const char *nl = memchr(start, '\n', (size_t)(end - start));
    const char *stop = nl ? nl : end;
    size_t len = (size_t)(stop - start);
    if (len > 0 && start[len - 1] == '\r') len--;
    *line = start;
    *line_len = len;
    *p = nl ? (size_t)(nl - body) + 1 : body_len;
    return 0;
}

/* Upper bound on the number of bytes a base64 value of length `len` (as
 * found on the wire, before whitespace/padding are stripped) could decode
 * to. Used to distinguish "value too big for the fixed output buffer"
 * (SDP_ERR_OVERFLOW) from "malformed base64" (SDP_ERR_PARSE) without
 * decoding twice. */
static size_t b64_decoded_upper_bound(const char *s, size_t len) {
    size_t sig = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        sig++;
    }
    return (sig * 6) / 8;
}

/* Locate a line "a=<key>:<value>" and return a pointer to <value> plus its
 * length. Returns NULL if no such attribute line exists. */
static const char *find_attr(const char *body, size_t body_len,
                              const char *key, size_t *val_len) {
    size_t klen = strlen(key);
    size_t p = 0;
    const char *line;
    size_t line_len;
    while (next_line(body, body_len, &p, &line, &line_len) == 0) {
        if (line_len > 2 + klen && line[0] == 'a' && line[1] == '=' &&
            memcmp(line + 2, key, klen) == 0 && line[2 + klen] == ':') {
            *val_len = line_len - (2 + klen + 1);
            return line + 2 + klen + 1;
        }
    }
    return NULL;
}

static void put_be32(uint8_t *p, int v) {
    p[0] = (uint8_t)((uint32_t)v >> 24);
    p[1] = (uint8_t)((uint32_t)v >> 16);
    p[2] = (uint8_t)((uint32_t)v >> 8);
    p[3] = (uint8_t)((uint32_t)v);
}

static void put_be16(uint8_t *p, int v) {
    p[0] = (uint8_t)((uint32_t)v >> 8);
    p[1] = (uint8_t)((uint32_t)v);
}

/* Build the 48-byte QuickTime 'frma'/'alac' ALAC magic cookie that
 * alac_set_info() expects. Byte layout ground-truthed against
 * tests/fixture_alac.c's COOKIE (which round-tripped through the real
 * decoder in foundations):
 *   [0:4)   size=12 (BE u32)         -- 'frma' atom size
 *   [4:8)   "frma"
 *   [8:12)  "alac"                    -- frma's data-format
 *   [12:16) size=36 (BE u32)         -- 'alac' config atom size
 *   [16:20) "alac"
 *   [20:24) version/flags = 0
 *   [24:28) frameLength    (BE u32) = f[0]
 *   [28]    compatibleVersion (u8)  = f[1]
 *   [29]    bitDepth          (u8)  = f[2]
 *   [30]    pb                (u8)  = f[3]
 *   [31]    mb                (u8)  = f[4]
 *   [32]    kb                (u8)  = f[5]
 *   [33]    numChannels       (u8)  = f[6]
 *   [34:36) maxRun         (BE u16) = f[7]
 *   [36:40) maxFrameBytes  (BE u32) = f[8]
 *   [40:44) avgBitRate     (BE u32) = f[9]
 *   [44:48) sampleRate     (BE u32) = f[10]
 */
static void build_cookie(const int f[11], uint8_t c[48]) {
    memset(c, 0, 48);
    c[3] = 12;
    memcpy(c + 4, "frma", 4);
    memcpy(c + 8, "alac", 4);
    c[15] = 36;
    memcpy(c + 16, "alac", 4);
    /* c[20..23] version/flags already zero */
    put_be32(c + 24, f[0]);
    c[28] = (uint8_t)f[1];
    c[29] = (uint8_t)f[2];
    c[30] = (uint8_t)f[3];
    c[31] = (uint8_t)f[4];
    c[32] = (uint8_t)f[5];
    c[33] = (uint8_t)f[6];
    put_be16(c + 34, f[7]);
    put_be32(c + 36, f[8]);
    put_be32(c + 40, f[9]);
    put_be32(c + 44, f[10]);
}

/* Parse an unsigned decimal integer from a bounded (not necessarily
 * NUL-terminated) range, rejecting overflow. On success *out is set and
 * *end_pos advances past the digits (may be NULL). Returns 0 on success. */
static int parse_uint_bounded(const char *p, const char *end, int *out,
                               const char **end_pos) {
    if (p >= end || *p < '0' || *p > '9') return -1;
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        int d = *p - '0';
        if (v > (LONG_MAX - d) / 10) return -1; /* overflow */
        v = v * 10 + d;
        p++;
    }
    if (v > INT_MAX) return -1;
    *out = (int)v;
    if (end_pos) *end_pos = p;
    return 0;
}

/* Parse exactly `count` whitespace-separated unsigned decimal integers from
 * a bounded range, requiring no other content before/after. Returns 0 on
 * success. */
static int parse_uint_list(const char *p, const char *end, int *vals,
                            int count) {
    for (int i = 0; i < count; i++) {
        while (p < end && *p == ' ') p++;
        const char *ep;
        if (parse_uint_bounded(p, end, &vals[i], &ep) != 0) return -1;
        p = ep;
    }
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return (p == end) ? 0 : -1; /* reject trailing garbage */
}

/* fmtp field ranges: u8 fields must fit a byte, maxRun fits u16, the rest
 * just need to be non-negative (already guaranteed by parse_uint_bounded)
 * and frameLength must be positive because session creation uses it to size buffers. */
static int fmtp_fields_in_range(const int f[11]) {
    if (f[0] <= 0 || f[0] > 16384) return -1;         /* frameLength: canonical 352/4096; cap kills 32-bit maxframe overflow */
    for (int i = 1; i <= 5; i++)                      /* compatVer..kb */
        if (f[i] < 0 || f[i] > 255) return -1;
    if (f[6] < 0 || f[6] > 255) return -1;             /* numChannels */
    if (f[7] < 0 || f[7] > 65535) return -1;           /* maxRun */
    return 0;                                          /* maxFrameBytes/avgBitRate/sampleRate: int range is enough */
}

int sdp_parse(const char *body, size_t body_len, raop_sdp_t *out) {
    memset(out, 0, sizeof *out);

    /* m=audio <port> RTP/AVP <payload-type> */
    {
        size_t p = 0;
        const char *line;
        size_t line_len;
        const char *m = NULL;
        size_t m_len = 0;
        while (next_line(body, body_len, &p, &line, &line_len) == 0) {
            if (line_len >= 8 && memcmp(line, "m=audio ", 8) == 0) {
                m = line;
                m_len = line_len;
                break;
            }
        }
        if (!m) return SDP_ERR_PARSE;
        static const char avp[] = "RTP/AVP ";
        const char *end = m + m_len;
        const char *hit = NULL;
        for (const char *q = m; q + (sizeof avp - 1) <= end; q++) {
            if (memcmp(q, avp, sizeof avp - 1) == 0) { hit = q; break; }
        }
        if (!hit) return SDP_ERR_PARSE;
        if (parse_uint_bounded(hit + (sizeof avp - 1), end, &out->payload_type, NULL) != 0)
            return SDP_ERR_PARSE;
    }

    /* fmtp: "a=fmtp:<pt> <frameLength> <compatVer> <bitDepth> <pb> <mb> <kb>
     *        <channels> <maxRun> <maxFrameBytes> <avgBitRate> <sampleRate>" */
    {
        size_t vl;
        const char *fm = find_attr(body, body_len, "fmtp", &vl);
        if (!fm) return SDP_ERR_PARSE;
        const char *fend = fm + vl;

        int all[12]; /* pt, then f[0..10] */
        if (parse_uint_list(fm, fend, all, 12) != 0) return SDP_ERR_FMTP;
        int pt = all[0];
        const int *f = all + 1;
        if (pt != out->payload_type) return SDP_ERR_FORMAT;
        if (fmtp_fields_in_range(f) != 0) return SDP_ERR_FMTP;

        out->frame_length = f[0];
        out->bit_depth = f[2];
        out->channels = f[6];
        out->sample_rate = f[10];
        if (out->channels != 2 || out->sample_rate != 44100 ||
            out->bit_depth != 16)
            return SDP_ERR_FORMAT;

        build_cookie(f, out->cookie);
        out->cookie_len = 48;
    }

    /* rsaaeskey: base64, decodes to 128 or 256 bytes (RSA modulus width). */
    {
        size_t vl;
        const char *rk = find_attr(body, body_len, "rsaaeskey", &vl);
        if (!rk) return SDP_ERR_PARSE;
        if (b64_decoded_upper_bound(rk, vl) > sizeof out->rsaaeskey)
            return SDP_ERR_OVERFLOW;
        if (b64_decode(rk, vl, out->rsaaeskey, sizeof out->rsaaeskey,
                        &out->rsaaeskey_len) != 0)
            return SDP_ERR_PARSE;
        if (out->rsaaeskey_len != 128 && out->rsaaeskey_len != 256)
            return SDP_ERR_KEYLEN;
    }

    /* aesiv: base64, must decode to exactly 16 bytes. */
    {
        size_t vl;
        const char *iv = find_attr(body, body_len, "aesiv", &vl);
        if (!iv) return SDP_ERR_PARSE;
        uint8_t ivbuf[32];
        size_t ivn;
        if (b64_decoded_upper_bound(iv, vl) > sizeof ivbuf)
            return SDP_ERR_OVERFLOW;
        if (b64_decode(iv, vl, ivbuf, sizeof ivbuf, &ivn) != 0)
            return SDP_ERR_PARSE;
        if (ivn != 16) return SDP_ERR_KEYLEN;
        memcpy(out->aesiv, ivbuf, 16);
    }

    return SDP_OK;
}
