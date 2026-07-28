#include "raop_clock.h"

uint64_t ntp_from_ns(uint64_t mono_ns, int64_t offset_ns){
    int64_t t=(int64_t)mono_ns+offset_ns; if (t<0) t=0;
    uint64_t sec=(uint64_t)t/1000000000ull;
    uint64_t frac=(uint64_t)t%1000000000ull;
    /* frac * 2^32 / 1e9 */
    uint64_t fixed=(frac<<32)/1000000000ull;
    return (sec<<32)|fixed;
}
int64_t ns_from_ntp(uint64_t ntp){
    uint64_t sec=ntp>>32, frac=ntp&0xFFFFFFFFull;
    return (int64_t)(sec*1000000000ull + (frac*1000000000ull>>32));
}
void clock_init(raop_clock_t *c){ c->offset_ns=0; c->best_rtt_ns=~0ull; c->have_offset=0; c->started=0; }

void clock_timing_reply(raop_clock_t *c, uint64_t t1_ns, uint64_t remote_ntp, uint64_t t4_ns){
    if (t4_ns<t1_ns) return;
    uint64_t rtt=t4_ns-t1_ns;
    if (c->have_offset && rtt>=c->best_rtt_ns) return;      /* keep lowest-RTT */
    int64_t mid=(int64_t)((t1_ns+t4_ns)/2);                 /* local midpoint  */
    int64_t remote_ns=ns_from_ntp(remote_ntp);
    c->offset_ns = remote_ns - mid;                          /* signed          */
    c->best_rtt_ns=rtt; c->have_offset=1;
}
clock_verdict_t clock_policy(raop_clock_t *c, long occ, long target, long margin){
    if (occ < target-margin || occ > target+margin){ c->started=0; return CLK_REBUFFER; }
    if (!c->started){ c->started=1; return CLK_START; }
    return CLK_CONTINUE;
}
