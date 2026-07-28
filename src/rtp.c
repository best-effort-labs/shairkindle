#include "rtp.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Bytewise helpers for all network-byte field access.
   ARMv6 faults on unaligned multi-byte loads; never cast to uint16_t or uint32_t pointers. */

static uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t rd_be64(const uint8_t *p) {
    return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4);
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, v >> 32);
    put_be32(p + 4, v);
}

/* Parse a RAOP audio RTP datagram. Returns 0 + fills out (payload points into buf), or -1 if
   too short / wrong version. Does NOT decrypt. */
int rtp_parse_audio(const uint8_t *buf, size_t len, rtp_audio_t *out) {
    /* Trust boundary check: enforce strict RAOP audio format.
       [0]=0x80 (V=2, no padding/extension/CSRC)
       [1]=0x60 (payload-type 96, may have marker)
       len>=28 (12-byte header + one mandatory 16-byte AES-CBC block)
     */
    if (len < 28 ||
        (buf[0] & 0xC0) != 0x80 ||  /* V=2 */
        (buf[0] & 0x3F) != 0 ||     /* no padding/extension/CSRC in byte 0 */
        (buf[1] & 0x7f) != 0x60) {  /* payload-type 96 (strip marker bit from byte 1) */
        return -1;
    }

    out->seq = rd_be16(buf + 2);
    out->rtptime = rd_be32(buf + 4);
    out->payload = buf + 12;
    out->payload_len = len - 12;
    return 0;
}

/* Classify a control-port datagram by its RTP payload-type byte. For CTRL_RESEND_AUDIO,
   inner and inner_len point at the embedded audio RTP packet (after the 4-byte resend header). */
ctrl_kind_t rtp_classify_control(const uint8_t *buf, size_t len, const uint8_t **inner, size_t *inner_len) {
    if (len < 2) {
        return CTRL_UNKNOWN;
    }

    uint8_t type = buf[1] & 0x7f;  /* strip marker bit */

    if (type == 0x54) {  /* 84 = sync */
        return CTRL_SYNC;
    }

    if (type == 0x56) {  /* 86 = resend response */
        if (len > 4) {
            *inner = buf + 4;
            *inner_len = len - 4;
            return CTRL_RESEND_AUDIO;
        }
        return CTRL_UNKNOWN;
    }

    return CTRL_UNKNOWN;
}

/* Parse a sync packet -> the current RTP timestamp it anchors + the NTP time. Returns 0/-1. */
int rtp_parse_sync(const uint8_t *buf, size_t len, uint32_t *rtp_now, uint64_t *ntp_now) {
    if (len < 16) {
        return -1;
    }

    *rtp_now = rd_be32(buf + 4);
    *ntp_now = rd_be64(buf + 8);
    return 0;
}

/* Build a resend request datagram for [first .. first+count-1]. Returns bytes written (8) or -1. */
int rtp_build_resend(uint16_t first, uint16_t count, uint8_t *out, size_t cap) {
    if (cap < 8) {
        return -1;
    }

    out[0] = 0x80;
    out[1] = 0xD5;  /* payload-type 85 + marker bit */
    put_be16(out + 2, 0x0001);  /* our seq */
    put_be16(out + 4, first);   /* missed first seq */
    put_be16(out + 6, count);   /* count */
    return 8;
}

/* Build a timing request datagram carrying our transmit NTP timestamp. Returns bytes (32) or -1. */
int rtp_build_timing_request(uint64_t t_tx_ntp, uint8_t *out, size_t cap) {
    if (cap < 32) {
        return -1;
    }

    memset(out, 0, 32);
    out[0] = 0x80;
    out[1] = 0xD2;  /* payload-type 82 + marker bit */
    put_be16(out + 2, 0x0007);  /* fixed field */
    /* [8:16] = zero (origin, already zeroed by memset) */
    /* [16:24] = zero (receive, already zeroed by memset) */
    put_be64(out + 24, t_tx_ntp);  /* our transmit NTP */
    return 32;
}

/* Parse a timing reply -> the sender's transmit NTP timestamp + the echoed originate token
   (our request's transmit NTP, echoed back at [8:16] per the RAOP/NTP-style exchange) so the
   caller can correlate the reply to a specific outstanding request. Returns 0/-1. */
int rtp_parse_timing_reply(const uint8_t *buf, size_t len, uint64_t *remote_tx_ntp,
                           uint64_t *origin_echo) {
    /* Must be exactly type 0x53 (83), reject any other type. */
    if (len < 32 || (buf[1] & 0x7f) != 0x53) {
        return -1;
    }

    *remote_tx_ntp = rd_be64(buf + 24);
    *origin_echo = rd_be64(buf + 8);
    return 0;
}

/* UDP socket helpers. */

/* Bind three consecutive-ish UDP sockets on 0.0.0.0 (ephemeral), non-blocking; fills the chosen
   ports. Returns 0/-1 (closes any partial open on failure). */
int rtp_open(rtp_sockets_t *s) {
    s->audio_fd = s->control_fd = s->timing_fd = -1;

    /* Open audio socket */
    s->audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->audio_fd < 0) {
        goto fail;
    }

    /* Open control socket */
    s->control_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->control_fd < 0) {
        goto fail;
    }

    /* Open timing socket */
    s->timing_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->timing_fd < 0) {
        goto fail;
    }

    /* Bind audio socket to ephemeral port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);

    if (bind(s->audio_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        goto fail;
    }

    /* Get the assigned port for audio */
    socklen_t addrlen = sizeof addr;
    if (getsockname(s->audio_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        goto fail;
    }
    s->server_port = ntohs(addr.sin_port);

    /* Bind control socket to ephemeral port */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);

    if (bind(s->control_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        goto fail;
    }

    /* Get the assigned port for control */
    addrlen = sizeof addr;
    if (getsockname(s->control_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        goto fail;
    }
    s->control_port = ntohs(addr.sin_port);

    /* Bind timing socket to ephemeral port */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);

    if (bind(s->timing_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        goto fail;
    }

    /* Get the assigned port for timing */
    addrlen = sizeof addr;
    if (getsockname(s->timing_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        goto fail;
    }
    s->timing_port = ntohs(addr.sin_port);

    /* Set all sockets to non-blocking */
    if (fcntl(s->audio_fd, F_SETFL, O_NONBLOCK) < 0) {
        goto fail;
    }
    if (fcntl(s->control_fd, F_SETFL, O_NONBLOCK) < 0) {
        goto fail;
    }
    if (fcntl(s->timing_fd, F_SETFL, O_NONBLOCK) < 0) {
        goto fail;
    }

    return 0;

fail:
    rtp_close(s);
    return -1;
}

/* Non-blocking recv from one fd into buf; returns bytes (>0), 0 if would-block, -1 error. Fills
   the sender's addr for peer validation. */
ssize_t rtp_recv(int fd, uint8_t *buf, size_t cap, struct sockaddr_in *from) {
    socklen_t fromlen = sizeof(*from);
    ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)from, &fromlen);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }

    return n;
}

/* Send a datagram to (ip,port). Returns bytes or -1. */
ssize_t rtp_sendto(int fd, const uint8_t *buf, size_t len, uint32_t ip_be, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip_be;
    addr.sin_port = htons(port);

    ssize_t n = sendto(fd, buf, len, 0, (struct sockaddr *)&addr, sizeof addr);
    if (n < 0) {
        return -1;
    }

    return n;
}

void rtp_close(rtp_sockets_t *s) {
    if (s->audio_fd >= 0) {
        close(s->audio_fd);
        s->audio_fd = -1;
    }
    if (s->control_fd >= 0) {
        close(s->control_fd);
        s->control_fd = -1;
    }
    if (s->timing_fd >= 0) {
        close(s->timing_fd);
        s->timing_fd = -1;
    }
}
