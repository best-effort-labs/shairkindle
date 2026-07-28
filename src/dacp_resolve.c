/* src/dacp_resolve.c — mDNS SRV query builder + hardened response parser (pure)
 * plus the one-shot socket resolver (device path). See dacp_resolve.h. */
#include "dacp_resolve.h"
#include "dacp.h"        /* mono_ms */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DNS_TYPE_SRV 33
#define DNS_MAX_NAME 256

/* Encode a dotted name into DNS label form at buf[0..cap). Returns bytes written
 * (incl. the root terminator) or -1. */
static int encode_name(unsigned char *buf, size_t cap, const char *dotted) {
    size_t o = 0;
    const char *s = dotted;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63) return -1;            /* empty/oversize label */
        if (o + 1 + l >= cap) return -1;
        buf[o++] = (unsigned char)l;
        memcpy(buf + o, s, l);
        o += l;
        if (!dot) break;
        s = dot + 1;
    }
    if (o + 1 > cap) return -1;
    buf[o++] = 0;                                    /* root */
    return (int)o;
}

int dacp_srv_build_query(unsigned char *buf, size_t cap, const char *owner, unsigned short dnsid) {
    if (!buf || !owner || cap < 12) return -1;
    buf[0] = (unsigned char)(dnsid >> 8); buf[1] = (unsigned char)dnsid;
    buf[2] = 0; buf[3] = 0;                           /* flags: standard query */
    buf[4] = 0; buf[5] = 1;                           /* qdcount = 1 */
    buf[6] = 0; buf[7] = 0;                           /* ancount */
    buf[8] = 0; buf[9] = 0;                           /* nscount */
    buf[10] = 0; buf[11] = 0;                         /* arcount */
    int n = encode_name(buf + 12, cap - 12, owner);
    if (n < 0) return -1;
    size_t o = 12 + (size_t)n;
    if (o + 4 > cap) return -1;
    buf[o++] = 0; buf[o++] = DNS_TYPE_SRV;            /* QTYPE = SRV */
    buf[o++] = 0x80; buf[o++] = 0x01;                 /* QCLASS = IN | QU (unicast-reply) */
    return (int)o;
}

/* Decode a DNS name at offset `off`, following compression pointers, into out
 * (dotted, lowercased). *next = offset just past the name at its FIRST wire
 * position (past the leading pointer if compressed). Returns 0 ok, -1 malformed. */
static int read_name(const unsigned char *p, size_t len, size_t off,
                     char *out, size_t outcap, size_t *next) {
    size_t o = 0;
    int hops = 0;
    int next_set = 0;
    size_t cur = off;
    for (;;) {
        if (cur >= len) return -1;
        unsigned char b = p[cur];
        if ((b & 0xC0) == 0xC0) {                     /* compression pointer */
            if (cur + 1 >= len) return -1;
            size_t ptr = ((size_t)(b & 0x3F) << 8) | p[cur + 1];
            if (!next_set) { *next = cur + 2; next_set = 1; }
            if (ptr >= cur) return -1;                /* must point backward (cycle guard) */
            if (++hops > 8) return -1;
            cur = ptr;
            continue;
        }
        if ((b & 0xC0) != 0) return -1;               /* reserved label type */
        size_t l = b;
        if (l == 0) {                                 /* root: end of name */
            if (!next_set) *next = cur + 1;
            if (o >= outcap) return -1;
            out[o] = 0;
            return 0;
        }
        if (l > 63) return -1;
        if (cur + 1 + l > len) return -1;
        if (o + l + 1 >= outcap) return -1;           /* +dot, leave room for NUL */
        if (o) out[o++] = '.';
        for (size_t i = 0; i < l; i++) {
            char c = (char)p[cur + 1 + i];
            out[o++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        cur += 1 + l;
    }
}

int dacp_srv_parse_response(const unsigned char *pkt, size_t len,
                            const char *owner, unsigned short dnsid, unsigned *port) {
    if (!pkt || !owner || !port || len < 12) return -1;
    unsigned short id = (unsigned short)((pkt[0] << 8) | pkt[1]);
    if (dnsid != 0 && id != dnsid) return -1;
    if (!(pkt[2] & 0x80)) return -1;                  /* QR must be 1 (a response) */
    unsigned qd = (unsigned)((pkt[4] << 8) | pkt[5]);
    unsigned an = (unsigned)((pkt[6] << 8) | pkt[7]);
    unsigned ns = (unsigned)((pkt[8] << 8) | pkt[9]);
    unsigned ar = (unsigned)((pkt[10] << 8) | pkt[11]);

    /* Lowercase the expected owner once for case-insensitive compare. */
    char want[DNS_MAX_NAME];
    size_t wl = strlen(owner);
    if (wl >= sizeof want) return -1;
    for (size_t i = 0; i <= wl; i++) {
        char c = owner[i];
        want[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }

    size_t off = 12;
    char nm[DNS_MAX_NAME];
    size_t next;

    /* Skip the question section: each = name + qtype(2) + qclass(2). */
    for (unsigned i = 0; i < qd; i++) {
        if (read_name(pkt, len, off, nm, sizeof nm, &next) != 0) return -1;
        off = next + 4;
        if (off > len) return -1;
    }

    unsigned total = an + ns + ar;
    for (unsigned i = 0; i < total; i++) {
        if (read_name(pkt, len, off, nm, sizeof nm, &next) != 0) return -1;
        off = next;
        if (off + 10 > len) return -1;
        unsigned type  = (unsigned)((pkt[off] << 8) | pkt[off + 1]);
        unsigned long ttl = ((unsigned long)pkt[off + 4] << 24) | ((unsigned long)pkt[off + 5] << 16) |
                            ((unsigned long)pkt[off + 6] << 8)  |  (unsigned long)pkt[off + 7];
        unsigned rdlen = (unsigned)((pkt[off + 8] << 8) | pkt[off + 9]);
        size_t rdata = off + 10;
        if (rdata + rdlen > len) return -1;
        if (type == DNS_TYPE_SRV && rdlen >= 6 && ttl != 0 && strcmp(nm, want) == 0) {
            unsigned p = (unsigned)((pkt[rdata + 4] << 8) | pkt[rdata + 5]);
            if (p != 0) { *port = p; return 0; }
        }
        off = rdata + rdlen;
    }
    return -1;
}

/* --- one-shot socket resolver (device path) --- */

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT  5353

unsigned dacp_resolve_port(const char *owner, unsigned iface_ip_be, long deadline_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    /* Bind an EPHEMERAL source port (NOT 5353 -- our own responder owns that;
     * a legacy ephemeral querier gets its unicast reply via the QU bit). */
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&me, sizeof me) != 0) { close(fd); return 0; }

    /* Pin multicast egress to the RAOP-selected interface (usb0 would otherwise
     * win). iface_ip_be is that interface's address in network byte order. */
    if (iface_ip_be) {
        struct in_addr ia; ia.s_addr = iface_ip_be;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia);
    }
    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);

    /* dnsid: low bits of the monotonic clock -- unique within the query window. */
    unsigned short dnsid = (unsigned short)(mono_ms() & 0xFFFF);
    if (dnsid == 0) dnsid = 1;

    unsigned char q[512];
    int qn = dacp_srv_build_query(q, sizeof q, owner, dnsid);
    if (qn < 0) { close(fd); return 0; }

    struct sockaddr_in grp = {0};
    grp.sin_family = AF_INET;
    grp.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    grp.sin_port = htons(MDNS_PORT);

    long start = mono_ms();
    long half_sent_at = 0;
    (void)sendto(fd, q, (size_t)qn, 0, (struct sockaddr *)&grp, sizeof grp);

    for (;;) {
        long now = mono_ms();
        long remaining = deadline_ms - now;
        if (remaining <= 0) break;
        /* second query near the halfway point (mitigates a single lost packet) */
        if (!half_sent_at && (now - start) >= (deadline_ms - start) / 2) {
            (void)sendto(fd, q, (size_t)qn, 0, (struct sockaddr *)&grp, sizeof grp);
            half_sent_at = now;
        }
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)(remaining > 1000 ? 1000 : remaining));
        if (pr <= 0) continue;
        unsigned char rb[1500];
        ssize_t n = recvfrom(fd, rb, sizeof rb, 0, NULL, NULL);
        if (n <= 0) continue;
        unsigned port = 0;
        /* accept both our legacy-unicast reply (id match) and a multicast one
         * (id 0 in a fresh mDNS response) -- pass dnsid=0 to skip the id gate,
         * relying on the exact owner-name match for correctness. */
        if (dacp_srv_parse_response(rb, (size_t)n, owner, 0, &port) == 0 && port) {
            close(fd);
            return port;
        }
    }
    close(fd);
    return 0;
}
