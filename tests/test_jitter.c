/* tests/test_jitter.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/jitter.h"

/* scripted deadline predicate: seqs listed as "expired" have passed deadline */
static uint64_t g_expired_upto = 0;
static int dl_passed(void *ud, uint64_t ext_seq){ (void)ud; return ext_seq < g_expired_upto; }

static int ins(jitter_t *j, uint16_t seq, uint32_t ts){
    uint8_t p[4]={ (uint8_t)seq,0,0,0 };
    return jitter_insert(j, seq, ts, p, sizeof p);
}

/* helper: insert a received packet at extended seq e (wire seq = e & 0xffff).
 * Named ins_e (not ins) -- this file already has an `ins(jitter_t*,uint16_t,uint32_t)`
 * helper with a different signature; C has no overloading. */
static void ins_e(jitter_t* j, uint64_t e){ uint8_t p[4]={1,2,3,4}; jitter_insert(j,(uint16_t)e,(uint32_t)(e*352),p,4); }

static void test_resync_fires_on_lap(void){
    jitter_t j; jitter_init(&j, 352, 0, 0);
    j.play_cursor = 100;                       /* stuck cursor */
    for (uint64_t e = 900; e < 1000; e++) ins_e(&j, e);   /* live edge ~900-999, far ahead */
    uint64_t skipped = 0;
    int r = jitter_try_resync(&j, &skipped);
    assert(r == 1);
    assert(j.play_cursor >= 900 - 100 && j.play_cursor <= 999);  /* jumped near live edge, w/ pre-roll */
    assert(skipped > 0);
    assert(j.resync_count == 1);
}

static void test_resync_noop_when_no_data_ahead(void){
    jitter_t j; jitter_init(&j, 352, 0, 0);
    j.play_cursor = 100;                       /* waiting for live packets; none far ahead */
    ins_e(&j, 101); ins_e(&j, 102);            /* small gap only, within window */
    uint64_t skipped = 0;
    assert(jitter_try_resync(&j, &skipped) == 0);   /* must NOT jump on a normal brief stall */
    assert(j.play_cursor == 100);
}

int main(void){
    jitter_t j; jitter_init(&j, 352, 100, 10000);
    jitter_set_deadline_passed(&j, dl_passed, NULL);
    jitter_slot_t out;

    /* in-order 100,101 -> pop both as DATA */
    assert(ins(&j,100,10000)==0);
    assert(ins(&j,101,10352)==0);
    assert(jitter_pop(&j,&out)==0 && out.kind==POP_DATA && out.ext_seq==100);
    assert(jitter_pop(&j,&out)==0 && out.ext_seq==101);

    /* gap: 102 missing, 103 arrives (reorder). pop stalls at 102 (not expired) */
    assert(ins(&j,103,10352*3-10352+10000)==0);   /* ts arbitrary */
    assert(jitter_pop(&j,&out)==1);               /* stall: waiting for 102 */
    /* a resend range should now cover [102,102] */
    uint64_t f,t; assert(jitter_next_resend(&j,&f,&t)==0 && f==102 && t==102);

    /* dupe of 103 -> ignored, counter++ */
    assert(ins(&j,103,0)==1 && j.dupes==1);

    /* 102 passes deadline -> pop yields CONCEAL, advances cursor + media time */
    g_expired_upto = 103;                          /* 102 now expired */
    assert(jitter_pop(&j,&out)==0 && out.kind==POP_CONCEAL && out.ext_seq==102 && out.frames==352);
    assert(j.concealed_count==1);
    /* now 103 pops as DATA */
    assert(jitter_pop(&j,&out)==0 && out.kind==POP_DATA && out.ext_seq==103);

    /* late retransmit of 102 arrives AFTER conceal -> discarded, late++ (no revive) */
    unsigned late_before = j.late;
    assert(ins(&j,102,0)==1);
    assert(j.late==late_before+1);
    /* Invariant 1, asserted directly: the concealed slot was NOT revived --
     * pop must proceed to the next real packet (104), never hand back
     * ext_seq=102 as POP_DATA. */
    assert(ins(&j,104,31056)==0);                  /* ts arbitrary */
    assert(jitter_pop(&j,&out)==0 && out.kind==POP_DATA && out.ext_seq==104);

    /* Invariant 3: FLUSH generation-invalidation under aliasing. The slot at
     * ext=102 (ring index 102) is still SLOT_CONCEALED from the old
     * generation. Flush to new_seq=614, whose ring index collides with it
     * (614 % 512 == 102). A fresh insert at the new boundary must pop as
     * its OWN packet -- the stale old-generation data at the same physical
     * index must not be mistaken for it -- and an insert below the new
     * boundary must still be rejected. */
    jitter_flush(&j, 614, 50000);
    assert(ins(&j,614,50000)==0);
    assert(jitter_pop(&j,&out)==0 && out.kind==POP_DATA && out.ext_seq==614);
    assert(ins(&j,613,0)==-1);                     /* below new boundary -> rejected */

    /* FLUSH to a new boundary 500: old slots gone, cursor rebased */
    jitter_flush(&j, 500, 99999);
    assert(ins(&j,499,0)==-1);                     /* below boundary -> rejected */
    assert(ins(&j,500,99999)==0);
    assert(jitter_pop(&j,&out)==0 && out.ext_seq==500);

    /* overwritten counter: a far-ahead insert aliasing an unconsumed slot
     * (ext=501 left RECEIVED/unconsumed, then ext=1013 lands on the same
     * physical ring index -- 1013 % 512 == 501 -- before 501 is popped). */
    assert(ins(&j,501,100351)==0);
    unsigned overwritten_before = j.overwritten;
    assert(ins(&j,1013,0)==0);
    assert(j.overwritten==overwritten_before+1);

    test_resync_fires_on_lap();
    test_resync_noop_when_no_data_ahead();

    printf("test_jitter OK\n");
    return 0;
}
