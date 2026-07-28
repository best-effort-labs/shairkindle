/* src/jitter.c */
#include "jitter.h"
#include <string.h>

static ring_slot_t *slot_for(jitter_t *j, uint64_t ext){ return &j->ring[ext % JITTER_SLOTS]; }

void jitter_init(jitter_t *j, int fpp, uint16_t ref_seq, uint32_t ref_ts){
    memset(j,0,sizeof *j);
    j->frames_per_pkt=fpp;
    seqext_init(&j->sx, ref_seq, ref_ts);
    j->play_cursor=ref_seq; j->boundary=ref_seq; j->gen=1;
}
void jitter_set_deadline_passed(jitter_t *j, int (*p)(void*,uint64_t), void *ud){
    j->deadline_passed=p; j->dl_ud=ud;
}

int jitter_insert(jitter_t *j, uint16_t wseq, uint32_t wts, const uint8_t *pl, size_t len){
    if (len>JITTER_MAX_PAYLOAD) return -1;
    uint64_t ext;
    if (seqext_seq(&j->sx,wseq,&ext)!=0) return -1;         /* ambiguous          */
    if (ext < j->boundary) { j->late++; return -1; }        /* pre-FLUSH boundary */
    if (ext < j->play_cursor){ j->late++; return 1; }       /* already played/concealed */
    ring_slot_t *s=slot_for(j,ext);
    if (s->gen==j->gen && s->ext_seq==ext &&
        (s->state==SLOT_RECEIVED)){ j->dupes++; return 1; } /* duplicate          */
    if (s->gen==j->gen && s->state==SLOT_RECEIVED && s->ext_seq!=ext) j->overwritten++;
    s->gen=j->gen; s->ext_seq=ext; s->state=SLOT_RECEIVED;
    s->rtp_ts=(uint32_t)seqext_ts(&j->sx,wts);
    memcpy(s->payload,pl,len); s->len=len;
    return 0;
}

int jitter_pop(jitter_t *j, jitter_slot_t *out){
    ring_slot_t *s=slot_for(j,j->play_cursor);
    int present = (s->gen==j->gen && s->ext_seq==j->play_cursor && s->state==SLOT_RECEIVED);
    if (present){
        out->kind=POP_DATA; out->ext_seq=j->play_cursor; out->rtp_ts=s->rtp_ts;
        out->frames=j->frames_per_pkt; out->len=s->len; memcpy(out->payload,s->payload,s->len);
        s->state=SLOT_CONSUMED; j->play_cursor++;
        return 0;
    }
    /* missing: conceal only if past deadline */
    if (j->deadline_passed && j->deadline_passed(j->dl_ud, j->play_cursor)){
        out->kind=POP_CONCEAL; out->ext_seq=j->play_cursor;
        out->rtp_ts=0; out->frames=j->frames_per_pkt; out->len=0;
        /* terminal concealed: mark slot, advance cursor + media time, cancel resend */
        s->gen=j->gen; s->ext_seq=j->play_cursor; s->state=SLOT_CONCEALED;
        j->concealed_count++; j->lost++; j->play_cursor++;
        return 0;
    }
    return 1;   /* stall */
}

void jitter_flush(jitter_t *j, uint16_t nseq, uint32_t nts){
    j->gen++;                                   /* invalidates every old slot */
    seqext_rebase(&j->sx,nseq,nts);
    j->play_cursor=nseq; j->boundary=nseq;
    /* resend/gap state is implicitly cleared by the generation bump + boundary */
}

int jitter_next_resend(jitter_t *j, uint64_t *from, uint64_t *to){
    /* scan forward from play_cursor for the first hole not received/concealed/consumed,
       coalesce the contiguous missing run. Cap the scan to the ring window. */
    uint64_t start=0; int in_hole=0;
    for (uint64_t e=j->play_cursor; e<j->play_cursor+JITTER_SLOTS; e++){
        ring_slot_t *s=slot_for(j,e);
        int received = (s->gen==j->gen && s->ext_seq==e &&
                        (s->state==SLOT_RECEIVED||s->state==SLOT_CONCEALED||s->state==SLOT_CONSUMED));
        /* only request holes that sit *before* some received packet (a real gap) */
        if (!received){ if(!in_hole){start=e;in_hole=1;} }
        else if (in_hole){ *from=start; *to=e-1; return 0; }
    }
    return -1;   /* no bounded hole */
}

/* A resync fires only when received data demonstrably sits far beyond the cursor:
 * the newest received ext_seq is > RESYNC_BEHIND packets ahead AND ends a contiguous
 * received run of >= RESYNC_MIN_RUN. We then set play_cursor to (run_start - PREROLL). */
#define RESYNC_BEHIND   128     /* > conceal window (62); < ring/2 */
#define RESYNC_MIN_RUN  8
/* INVARIANT: RESYNC_PREROLL must stay strictly below the player's conceal window
 * (session.c: START_FILL_FRAMES/frames_per_pkt, currently 22050/352 =~ 62). The
 * preroll slots left before play_cursor after a resync jump are survived only by
 * concealment, not by stalling -- if the conceal window shrinks to <= RESYNC_PREROLL,
 * those slots neither pop nor conceal, so the player stalls repeatedly right after its
 * own resync. session.c's pl_thread decrements resync_hold once per loop iteration
 * regardless of outcome (not just on POP_DATA/POP_CONCEAL), so this no longer wedges
 * permanently -- it re-arms and retries resync every ~64 iterations -- but it still
 * means audible churn instead of clean recovery. Safe today (16 < 62), but a future
 * latency-tuning change to START_FILL_FRAMES could trip it. */
#define RESYNC_PREROLL  16

int jitter_try_resync(jitter_t *j, uint64_t *skipped){
    /* 1. highest valid received ext_seq anywhere in the ring */
    uint64_t hi = 0; int found = 0;
    for (int i = 0; i < JITTER_SLOTS; i++){
        ring_slot_t *s = &j->ring[i];
        if (s->gen == j->gen && s->state == SLOT_RECEIVED && s->ext_seq >= j->play_cursor){
            if (!found || s->ext_seq > hi){ hi = s->ext_seq; found = 1; }
        }
    }
    if (!found || hi < j->play_cursor + RESYNC_BEHIND) return 0;   /* not lapped -> keep stalling */

    /* 2. walk back from hi while slots stay received+contiguous -> run_start */
    uint64_t run_start = hi;
    while (run_start > j->play_cursor){
        ring_slot_t *s = &j->ring[(run_start - 1) % JITTER_SLOTS];
        if (s->gen == j->gen && s->ext_seq == (run_start - 1) && s->state == SLOT_RECEIVED) run_start--;
        else break;
    }
    if (hi - run_start + 1 < RESYNC_MIN_RUN) return 0;             /* run too short -> not trustworthy */

    /* 3. jump, preserving a small pre-roll (never behind the old cursor) */
    uint64_t target = run_start > RESYNC_PREROLL ? run_start - RESYNC_PREROLL : run_start;
    if (target < j->play_cursor) target = j->play_cursor;
    *skipped = target - j->play_cursor;
    j->play_cursor = target;
    j->resync_count++; j->resync_skipped += *skipped;
    return 1;
}
