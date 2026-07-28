#include "dacp_resolve.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* Append a DNS name (labels, no compression) terminated by a root label. */
static size_t put_name(unsigned char *p, const char *dotted) {
    size_t o = 0; const char *s = dotted;
    while (*s) {
        const char *dot = strchr(s, '.'); size_t l = dot ? (size_t)(dot - s) : strlen(s);
        p[o++] = (unsigned char)l; memcpy(p + o, s, l); o += l;
        if (!dot) break;
        s = dot + 1;
    }
    p[o++] = 0; return o;
}

/* --- Case A: uncompressed owner, SRV in the ANSWER section --- */
static size_t build_answer_pkt(unsigned char *pkt, const char *owner, unsigned port) {
    size_t o = 0;
    unsigned char hdr[12] = {0x12,0x34, 0x84,0x00, 0,0, 0,1, 0,0, 0,0}; /* QR=1, an=1 */
    memcpy(pkt, hdr, 12); o = 12;
    o += put_name(pkt + o, owner);
    unsigned char rr[] = {0,33, 0,1, 0,0,0,120, 0,0}; size_t rrpos = o;
    memcpy(pkt + o, rr, sizeof rr); o += sizeof rr;
    unsigned char rd[] = {0,0, 0,0, (unsigned char)(port>>8),(unsigned char)port, 0};
    memcpy(pkt + o, rd, sizeof rd); o += sizeof rd;
    pkt[rrpos + 8] = 0; pkt[rrpos + 9] = (unsigned char)sizeof rd;   /* rdlen */
    return o;
}

/* --- Case B: owner in the QUESTION, SRV in the ADDITIONAL section with the
 *     owner name COMPRESSED (pointer back to offset 12). --- */
static size_t build_compressed_pkt(unsigned char *pkt, const char *owner, unsigned port) {
    size_t o = 0;
    unsigned char hdr[12] = {0xAB,0xCD, 0x84,0x00, 0,1, 0,0, 0,0, 0,1}; /* QR=1, qd=1, ar=1 */
    memcpy(pkt, hdr, 12); o = 12;
    size_t name_off = o;
    o += put_name(pkt + o, owner);                 /* question name at offset 12 */
    pkt[o++] = 0; pkt[o++] = 33;                    /* qtype SRV */
    pkt[o++] = 0x80; pkt[o++] = 0x01;               /* qclass IN|QU */
    /* additional RR: owner as compression pointer -> name_off */
    pkt[o++] = 0xC0; pkt[o++] = (unsigned char)name_off;
    unsigned char rr[] = {0,33, 0,1, 0,0,0,120, 0,0}; size_t rrpos = o;
    memcpy(pkt + o, rr, sizeof rr); o += sizeof rr;
    unsigned char rd[] = {0,0, 0,0, (unsigned char)(port>>8),(unsigned char)port, 0};
    memcpy(pkt + o, rd, sizeof rd); o += sizeof rd;
    pkt[rrpos + 8] = 0; pkt[rrpos + 9] = (unsigned char)sizeof rd;
    return o;
}

int main(void) {
    const char *owner = "iTunes_Ctrl_56B2A4D4._dacp._tcp.local";
    unsigned char pkt[512];
    unsigned port = 0;

    /* Case A */
    size_t a = build_answer_pkt(pkt, owner, 49372);
    assert(dacp_srv_parse_response(pkt, a, owner, 0x1234, &port) == 0 && port == 49372);
    port = 0;
    assert(dacp_srv_parse_response(pkt, a, "itunes_ctrl_56b2a4d4._dacp._tcp.local", 0, &port) == 0
           && port == 49372);                                   /* case-insensitive */
    assert(dacp_srv_parse_response(pkt, a, "iTunes_Ctrl_DEADBEEF._dacp._tcp.local", 0, &port) == -1);
    assert(dacp_srv_parse_response(pkt, a, owner, 0x9999, &port) == -1);  /* wrong dnsid */
    for (size_t t = 0; t < a; t++)                              /* truncation: never crash */
        assert(dacp_srv_parse_response(pkt, t, owner, 0, &port) == -1);

    /* Case B: compression pointer + SRV in the additional section */
    port = 0;
    size_t b = build_compressed_pkt(pkt, owner, 51001);
    assert(dacp_srv_parse_response(pkt, b, owner, 0xABCD, &port) == 0 && port == 51001);
    for (size_t t = 0; t < b; t++)
        assert(dacp_srv_parse_response(pkt, t, owner, 0, &port) == -1);

    /* port 0 rejected */
    size_t z = build_answer_pkt(pkt, owner, 0);
    assert(dacp_srv_parse_response(pkt, z, owner, 0, &port) == -1);

    /* build query well-formed: id echoed, one question */
    unsigned char q[256];
    int qn = dacp_srv_build_query(q, sizeof q, owner, 0xABCD);
    assert(qn > 12);
    assert(q[0] == 0xAB && q[1] == 0xCD);
    assert(q[4] == 0 && q[5] == 1);
    assert(q[qn - 4] == 0 && q[qn - 3] == 33);      /* QTYPE SRV */
    assert(q[qn - 2] == 0x80 && q[qn - 1] == 0x01); /* QCLASS IN|QU */

    printf("test_dacp_srv: OK\n");
    return 0;
}
