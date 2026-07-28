#include "supervisor.h"
#include <string.h>
#include <stdio.h>

int sup_pidfile_format(char *buf, size_t cap, long pid, unsigned long long start) {
    int n = snprintf(buf, cap, "%ld %llu\n", pid, start);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int sup_stat_starttime(const char *stat, unsigned long long *out) {
    const char *p = strrchr(stat, ')');   /* skip comm; it may contain ')' */
    if (!p) return -1;
    p++;                                    /* first token here is field 3 (state) */
    int tok = 0;                            /* field 22 == 20th token after ')'    */
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok++;
        if (tok == 20) return sscanf(p, "%llu", out) == 1 ? 0 : -1;
        while (*p && *p != ' ') p++;
    }
    return -1;
}

void sup_state_init(sup_state_t *s) {
    s->phase = SUP_IDLE;
    s->session_id = 0;
    s->last_hb_ms = 0;
}

sup_action_t sup_step(sup_state_t *s, sup_event_t ev, int64_t now_ms) {
    switch (ev) {
    case SUP_EV_ACCEPT:
        /* precondition: caller (accept loop) only issues ACCEPT from IDLE.
         * Defensive: a repeat still bumps the session and re-arms. */
        s->phase = SUP_ACTIVE;
        s->session_id++;
        s->last_hb_ms = now_ms;
        return SUP_ACT_START;
    case SUP_EV_HEARTBEAT:
    /* DACP command tokens double as proof-of-life: using the remote keeps the
     * lease alive. The relay (sendto to raopd) is done by the accept loop off
     * the event value; sup_step just refreshes the heartbeat. */
    case SUP_EV_NEXT:
    case SUP_EV_PREV:
    case SUP_EV_PLAYPAUSE:
        if (s->phase == SUP_ACTIVE) s->last_hb_ms = now_ms;
        return SUP_ACT_NONE;
    case SUP_EV_STOP:
    case SUP_EV_EOF:
        if (s->phase == SUP_ACTIVE) { s->phase = SUP_IDLE; return SUP_ACT_STOP; }
        return SUP_ACT_NONE;
    case SUP_EV_TICK:
        if (s->phase == SUP_ACTIVE &&
            now_ms - s->last_hb_ms > SUP_HEARTBEAT_TIMEOUT_MS) {
            s->phase = SUP_IDLE;
            return SUP_ACT_STOP;
        }
        return SUP_ACT_NONE;
    case SUP_EV_NONE:
    default:
        return SUP_ACT_NONE;
    }
}

sup_event_t sup_classify_line(const char *line) {
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
    if (n == 4 && strncmp(line, "STOP", 4) == 0) return SUP_EV_STOP;
    if (n == 4 && strncmp(line, "PING", 4) == 0) return SUP_EV_HEARTBEAT;
    if (n >= 5 && strncmp(line, "HELLO", 5) == 0) return SUP_EV_HEARTBEAT;
    if (n == 4 && strncmp(line, "NEXT", 4) == 0) return SUP_EV_NEXT;
    if (n == 4 && strncmp(line, "PREV", 4) == 0) return SUP_EV_PREV;
    if (n == 9 && strncmp(line, "PLAYPAUSE", 9) == 0) return SUP_EV_PLAYPAUSE;
    return SUP_EV_NONE;
}

void sup_lb_init(sup_lb_t *lb) { lb->used = 0; lb->discarding = 0; }

int sup_lb_push(sup_lb_t *lb, const char *data, size_t n,
                sup_event_t *evs, int max) {
    int ne = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (lb->discarding) {           /* swallow the rest of an overlong line */
            if (c == '\n') lb->discarding = 0;
            continue;
        }
        if (c == '\n') {
            lb->buf[lb->used] = 0;
            if (ne < max) evs[ne++] = sup_classify_line(lb->buf);
            lb->used = 0;
            continue;
        }
        if (lb->used < sizeof lb->buf - 1) {
            lb->buf[lb->used++] = c;
        } else {                        /* overlong: drop the WHOLE line */
            lb->discarding = 1;
            lb->used = 0;
        }
    }
    return ne;
}
