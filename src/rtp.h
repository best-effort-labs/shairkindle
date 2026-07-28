/* src/rtp.h — RTP wire parsing/building and UDP socket helpers. */
#ifndef SHAIRKINDLE_RTP_H
#define SHAIRKINDLE_RTP_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "raop_clock.h"

typedef struct { uint16_t seq; uint32_t rtptime; const uint8_t *payload; size_t payload_len; } rtp_audio_t;

/* Parse a RAOP audio RTP datagram. Returns 0 + fills out (payload points into buf), or -1 if
   too short (len<28) / wrong version. Does NOT decrypt. */
int rtp_parse_audio(const uint8_t *buf, size_t len, rtp_audio_t *out);

typedef enum { CTRL_SYNC, CTRL_RESEND_AUDIO, CTRL_UNKNOWN } ctrl_kind_t;

/* Classify a control-port datagram by its RTP payload-type byte. For CTRL_RESEND_AUDIO,
   inner and inner_len point at the embedded audio RTP packet (after the 4-byte resend header). */
ctrl_kind_t rtp_classify_control(const uint8_t *buf, size_t len, const uint8_t **inner, size_t *inner_len);

/* Parse a sync packet -> the current RTP timestamp it anchors + the NTP time. Returns 0/-1. */
int rtp_parse_sync(const uint8_t *buf, size_t len, uint32_t *rtp_now, uint64_t *ntp_now);

/* Build a resend request datagram for [first .. first+count-1]. Returns bytes written (8) or -1. */
int rtp_build_resend(uint16_t first, uint16_t count, uint8_t *out, size_t cap);

/* Build a timing request datagram carrying our transmit NTP timestamp. Returns bytes (32) or -1. */
int rtp_build_timing_request(uint64_t t_tx_ntp, uint8_t *out, size_t cap);

/* Parse a timing reply -> the sender's transmit NTP timestamp + the echoed originate token
   (our request's transmit NTP, echoed back at [8:16]) so the caller can correlate the reply
   to the specific outstanding request instead of accepting any reply from the sender IP.
   Returns 0/-1. */
int rtp_parse_timing_reply(const uint8_t *buf, size_t len, uint64_t *remote_tx_ntp,
                           uint64_t *origin_echo);

/* UDP socket helpers. */

typedef struct {
    int audio_fd, control_fd, timing_fd;
    int server_port, control_port, timing_port;
} rtp_sockets_t;

/* Bind three consecutive-ish UDP sockets on 0.0.0.0 (ephemeral), non-blocking; fills the chosen
   ports. Returns 0/-1 (closes any partial open on failure). */
int rtp_open(rtp_sockets_t *s);

/* Non-blocking recv from one fd into buf; returns bytes (>0), 0 if would-block, -1 error. Fills
   the sender's addr for peer validation. */
ssize_t rtp_recv(int fd, uint8_t *buf, size_t cap, struct sockaddr_in *from);

/* Send a datagram to (ip,port). Returns bytes or -1. */
ssize_t rtp_sendto(int fd, const uint8_t *buf, size_t len, uint32_t ip_be, uint16_t port);

void rtp_close(rtp_sockets_t *s);

#endif
