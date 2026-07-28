#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "../src/supervisor.h"

int main(void) {
    sup_state_t s;
    sup_state_init(&s);
    assert(s.phase == SUP_IDLE);
    assert(s.session_id == 0);

    /* accept -> START, becomes ACTIVE, session 1, heartbeat stamped */
    assert(sup_step(&s, SUP_EV_ACCEPT, 1000) == SUP_ACT_START);
    assert(s.phase == SUP_ACTIVE);
    assert(s.session_id == 1);

    /* a tick just before timeout (no heartbeat since accept) -> still up */
    assert(sup_step(&s, SUP_EV_TICK, 1000 + SUP_HEARTBEAT_TIMEOUT_MS) == SUP_ACT_NONE);
    assert(s.phase == SUP_ACTIVE);

    /* heartbeat refreshes the deadline */
    assert(sup_step(&s, SUP_EV_HEARTBEAT, 1000 + SUP_HEARTBEAT_TIMEOUT_MS) == SUP_ACT_NONE);
    int64_t hb = 1000 + SUP_HEARTBEAT_TIMEOUT_MS;
    /* a tick within the window from the LAST heartbeat does not time out */
    assert(sup_step(&s, SUP_EV_TICK, hb + SUP_HEARTBEAT_TIMEOUT_MS) == SUP_ACT_NONE);
    assert(s.phase == SUP_ACTIVE);
    /* a tick strictly past the window -> STOP, back to IDLE */
    assert(sup_step(&s, SUP_EV_TICK, hb + SUP_HEARTBEAT_TIMEOUT_MS + 1) == SUP_ACT_STOP);
    assert(s.phase == SUP_IDLE);

    /* strays while idle are no-ops */
    assert(sup_step(&s, SUP_EV_HEARTBEAT, 99999) == SUP_ACT_NONE);
    assert(sup_step(&s, SUP_EV_TICK, 99999) == SUP_ACT_NONE);
    assert(sup_step(&s, SUP_EV_STOP, 99999) == SUP_ACT_NONE);

    /* explicit STOP tears down; session id keeps climbing across cycles */
    assert(sup_step(&s, SUP_EV_ACCEPT, 200000) == SUP_ACT_START);
    assert(s.session_id == 2);
    assert(sup_step(&s, SUP_EV_STOP, 200500) == SUP_ACT_STOP);
    assert(s.phase == SUP_IDLE);

    /* EOF is a teardown too */
    assert(sup_step(&s, SUP_EV_ACCEPT, 300000) == SUP_ACT_START);
    assert(s.session_id == 3);
    assert(sup_step(&s, SUP_EV_EOF, 300100) == SUP_ACT_STOP);
    assert(s.phase == SUP_IDLE);

    /* line classification */
    assert(sup_classify_line("PING")    == SUP_EV_HEARTBEAT);
    assert(sup_classify_line("PING\r")  == SUP_EV_HEARTBEAT);
    assert(sup_classify_line("HELLO 1") == SUP_EV_HEARTBEAT);
    assert(sup_classify_line("STOP")    == SUP_EV_STOP);
    assert(sup_classify_line("garbage") == SUP_EV_NONE);
    assert(sup_classify_line("")        == SUP_EV_NONE);

    /* DACP command tokens (v2) classify to distinct events; near-prefixes don't */
    assert(sup_classify_line("NEXT")      == SUP_EV_NEXT);
    assert(sup_classify_line("PREV")      == SUP_EV_PREV);
    assert(sup_classify_line("PLAYPAUSE") == SUP_EV_PLAYPAUSE);
    assert(sup_classify_line("NEXT\r")    == SUP_EV_NEXT);
    assert(sup_classify_line("NEXTjunk")  == SUP_EV_NONE);   /* length guard */
    assert(sup_classify_line("PLAY")      == SUP_EV_NONE);

    /* command tokens refresh the lease heartbeat like PING (no action returned) */
    {
        sup_state_t cs; sup_state_init(&cs);
        assert(sup_step(&cs, SUP_EV_ACCEPT, 1000) == SUP_ACT_START);
        assert(sup_step(&cs, SUP_EV_NEXT, 1000 + SUP_HEARTBEAT_TIMEOUT_MS) == SUP_ACT_NONE);
        /* the NEXT refreshed the deadline -> a tick within the window holds */
        assert(sup_step(&cs, SUP_EV_TICK, 1000 + 2*SUP_HEARTBEAT_TIMEOUT_MS) == SUP_ACT_NONE);
        assert(cs.phase == SUP_ACTIVE);
    }

    /* --- line buffer --- */
    sup_lb_t lb; sup_event_t evs[8]; int ne;
    sup_lb_init(&lb);
    ne = sup_lb_push(&lb, "PING\nSTOP\n", 10, evs, 8);
    assert(ne == 2 && evs[0] == SUP_EV_HEARTBEAT && evs[1] == SUP_EV_STOP);

    /* a batch mixing heartbeat + command tokens preserves order + identity */
    sup_lb_init(&lb);
    ne = sup_lb_push(&lb, "PING\nNEXT\nPLAYPAUSE\n", 20, evs, 8);
    assert(ne == 3 && evs[0] == SUP_EV_HEARTBEAT && evs[1] == SUP_EV_NEXT && evs[2] == SUP_EV_PLAYPAUSE);

    /* fragmentation: a line split across two reads yields one event */
    sup_lb_init(&lb);
    assert(sup_lb_push(&lb, "PI", 2, evs, 8) == 0);
    ne = sup_lb_push(&lb, "NG\n", 3, evs, 8);
    assert(ne == 1 && evs[0] == SUP_EV_HEARTBEAT);

    /* overlong line must be dropped WHOLE -- a trailing "STOP" inside it is NOT
     * recognized as a command (the suffix-injection bug) */
    sup_lb_init(&lb);
    {
        char big[700];
        memset(big, 'x', 600);
        memcpy(big + 600, "STOP\n", 5);          /* 605 bytes, one overlong line */
        ne = sup_lb_push(&lb, big, 605, evs, 8);
        assert(ne == 0);                          /* no STOP leaked out */
    }
    /* and the buffer recovers: the next well-formed line still parses */
    ne = sup_lb_push(&lb, "PING\n", 5, evs, 8);
    assert(ne == 1 && evs[0] == SUP_EV_HEARTBEAT);

    /* --- pidfile format (pure) --- */
    {
        char pb[64];
        int n = sup_pidfile_format(pb, sizeof pb, 1234, 987654ULL);
        assert(n > 0 && strcmp(pb, "1234 987654\n") == 0);
        /* overflow -> -1, buffer not relied upon */
        assert(sup_pidfile_format(pb, 4, 1234, 987654ULL) == -1);
    }
    /* --- /proc/<pid>/stat starttime parse (field 22, comm may hold spaces+parens) --- */
    {
        unsigned long long st = 0;
        const char *line =
          "1234 (rao) pd) S 1 1234 1234 0 -1 4194560 100 0 0 0 5 6 0 0 20 0 1 0 987654 rest";
        assert(sup_stat_starttime(line, &st) == 0 && st == 987654ULL);
        assert(sup_stat_starttime("garbage-no-paren", &st) != 0);
    }

    printf("test_supervisor OK\n");
    return 0;
}
