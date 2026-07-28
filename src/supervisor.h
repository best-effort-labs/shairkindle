#ifndef SHAIRKINDLE_SUPERVISOR_H
#define SHAIRKINDLE_SUPERVISOR_H
#include <stdint.h>
#include <stddef.h>

/* Lease state machine + line reassembly for airplay-supervisor. The kindlet's
 * live control connection IS the "receiver should be up" signal; loss of it
 * (EOF, heartbeat timeout, or explicit STOP) tears the receiver down. Pure
 * logic — no sockets. Time is int64 ms (ARMv6 `long` is 32-bit → ms overflow). */

#define SUP_HEARTBEAT_TIMEOUT_MS 30000LL   /* expires strictly AFTER 30 s silence */
#define SUP_PORT 5566
#define SUP_LB_MAX 512                      /* max control-line length incl. NUL */

typedef enum { SUP_IDLE = 0, SUP_ACTIVE = 1 } sup_phase_t;

typedef enum {
    SUP_EV_ACCEPT,     /* a new owner connection was accepted */
    SUP_EV_HEARTBEAT,  /* a keepalive line (HELLO/PING) arrived */
    SUP_EV_STOP,       /* explicit STOP line */
    SUP_EV_EOF,        /* connection closed/reset */
    SUP_EV_TICK,       /* periodic timer tick */
    SUP_EV_NEXT,       /* DACP command tokens (v2) -- also refresh the lease  */
    SUP_EV_PREV,
    SUP_EV_PLAYPAUSE,
    SUP_EV_NONE        /* nothing actionable (unknown line) */
} sup_event_t;

typedef enum { SUP_ACT_NONE, SUP_ACT_START, SUP_ACT_STOP } sup_action_t;

typedef struct {
    sup_phase_t phase;
    unsigned    session_id;   /* ++ each accept; monotonic across cycles */
    int64_t     last_hb_ms;   /* monotonic ms of the last accept/heartbeat */
} sup_state_t;

/* Byte-stream -> newline-delimited lines -> events. Recovers from overlong
 * lines by discarding the whole line (up to the next '\n'). */
typedef struct {
    char   buf[SUP_LB_MAX];
    size_t used;
    int    discarding;        /* inside an overlong line: swallow to next '\n' */
} sup_lb_t;

void         sup_state_init(sup_state_t *s);
sup_action_t sup_step(sup_state_t *s, sup_event_t ev, int64_t now_ms);
sup_event_t  sup_classify_line(const char *line);

void         sup_lb_init(sup_lb_t *lb);
/* Push n received bytes; append up to `max` classified events into evs[].
 * Returns the number of events emitted. */
int          sup_lb_push(sup_lb_t *lb, const char *data, size_t n,
                         sup_event_t *evs, int max);

/* Format pidfile CONTENTS "<pid> <starttime>\n". Returns bytes written, or -1 on
 * overflow. Pure (no IO) so it is unit-testable. */
int sup_pidfile_format(char *buf, size_t cap, long pid, unsigned long long start);

/* Parse starttime (field 22) from a /proc/<pid>/stat line. comm (field 2) is
 * parenthesized and may contain spaces AND ')', so scan from the LAST ')'.
 * Returns 0 on success, non-zero on malformed input. */
int sup_stat_starttime(const char *stat, unsigned long long *out);

#endif
