#ifndef SHAIRKINDLE_RAOP_CLOCK_H
#define SHAIRKINDLE_RAOP_CLOCK_H
#include <stdint.h>
typedef enum { CLK_START, CLK_CONTINUE, CLK_REBUFFER } clock_verdict_t;
typedef struct {
    int64_t  offset_ns;      /* signed remote-minus-local estimate       */
    uint64_t best_rtt_ns;    /* lowest RTT seen -> most trusted sample    */
    int      have_offset;
    int      started;
} raop_clock_t;
uint64_t ntp_from_ns(uint64_t mono_ns, int64_t offset_ns);
int64_t  ns_from_ntp(uint64_t ntp);
void     clock_init(raop_clock_t *c);
void     clock_timing_reply(raop_clock_t *c, uint64_t t1_ns, uint64_t remote_ntp, uint64_t t4_ns);
clock_verdict_t clock_policy(raop_clock_t *c, long occupancy_frames,
                             long target_frames, long margin_frames);
#endif
