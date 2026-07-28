/* src/rtsp_msg.h — RTSP request wire framing (request line, headers,
 * Content-Length body). Pure parse: caller owns the byte buffer and keeps it
 * alive as long as the returned rtsp_msg_t's header/body pointers are used
 * (they point INTO buf, nothing is copied). Supports incremental feed: a
 * partial message returns "need more bytes" (0) rather than erroring, so a
 * caller can top up its buffer and retry.
 */
#ifndef SHAIRKINDLE_RTSP_MSG_H
#define SHAIRKINDLE_RTSP_MSG_H
#include <stddef.h>

#define RTSP_MAX_HDRS 24

typedef struct { const char *name; size_t nlen; const char *val; size_t vlen; } rtsp_hdr_t;

typedef struct {
    const char *method; size_t method_len;
    const char *uri;    size_t uri_len;
    rtsp_hdr_t  hdr[RTSP_MAX_HDRS]; int n_hdr;
    const char *body;   size_t body_len;
    long        cseq;   /* parsed CSeq, -1 if absent or unparseable */
} rtsp_msg_t;

/* Parse one RTSP request out of buf[0..len). Returns:
 *   1  - complete message parsed; *consumed = bytes eaten (request line +
 *        headers + Content-Length body). Call again on buf+*consumed to
 *        parse a second message packed into the same buffer.
 *   0  - need more bytes (partial request line/headers/body); *consumed
 *        untouched, caller should top up buf and retry.
 *  -1  - malformed (bad request line, or an unparseable/negative/oversized
 *        Content-Length); *consumed untouched.
 */
int  rtsp_msg_parse(const char *buf, size_t len, rtsp_msg_t *m, size_t *consumed);

/* Case-insensitive header lookup. name must be NUL-terminated; NULL if absent.
 * The returned value pointer is NOT NUL-terminated -- it points into buf and
 * is bounded only by the matching rtsp_hdr_t.vlen, so callers must not
 * strcmp/strlen/strstr it directly. */
const char *rtsp_hdr(const rtsp_msg_t *m, const char *name);

#endif
