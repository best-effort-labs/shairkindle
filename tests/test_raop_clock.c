#include <assert.h>
#include <stdio.h>
#include "../src/raop_clock.h"

int main(void){
    /* NTP round-trip: 1.5s -> 32.32 fixed -> back */
    uint64_t ntp = ntp_from_ns(1500000000ull, 0);
    assert((ntp>>32)==1);                                   /* 1 second integer  */
    assert(ns_from_ntp(ntp) > 1499000000 && ns_from_ntp(ntp) < 1501000000);

    raop_clock_t c; clock_init(&c);
    /* a reply with RTT 20ms; remote clock ~ +500ms ahead. */
    uint64_t t1=1000000000ull, t4=1020000000ull;            /* 20ms RTT          */
    uint64_t remote=ntp_from_ns(1510000000ull,0);           /* remote "now"      */
    clock_timing_reply(&c,t1,remote,t4);
    assert(c.have_offset);
    int64_t off1=c.offset_ns;
    /* a worse-RTT sample (200ms) must NOT replace the good one */
    clock_timing_reply(&c,2000000000ull,ntp_from_ns(2600000000ull,0),2200000000ull);
    assert(c.offset_ns==off1);                              /* kept lowest-RTT   */

    /* policy: below target-margin => REBUFFER; at/above target => START then CONTINUE */
    assert(clock_policy(&c, 100,  88200, 20000)==CLK_REBUFFER);   /* far under  */
    assert(clock_policy(&c, 88200,88200, 20000)==CLK_START);      /* reached    */
    assert(clock_policy(&c, 88200,88200, 20000)==CLK_CONTINUE);   /* already started */
    assert(clock_policy(&c, 200000,88200,20000)==CLK_REBUFFER);   /* far over   */
    printf("test_raop_clock OK\n");
    return 0;
}
