/* src/jitter.h */
#ifndef SHAIRKINDLE_JITTER_H
#define SHAIRKINDLE_JITTER_H
#include <stddef.h>
#include <stdint.h>
#include "seqext.h"
#define JITTER_SLOTS 512
#define JITTER_MAX_PAYLOAD 2048
typedef enum { SLOT_EMPTY, SLOT_WAITING, SLOT_RESEND, SLOT_RECEIVED,
               SLOT_CONCEALED, SLOT_CONSUMED } slot_state_t;
typedef enum { POP_DATA, POP_CONCEAL } pop_kind_t;
typedef struct {
    pop_kind_t kind;
    uint64_t   ext_seq;
    uint32_t   rtp_ts;          /* POP_DATA: actual. POP_CONCEAL: 0/unused --
                                   media time advances via `frames`, not rtp_ts */
    int        frames;          /* negotiated frames to advance media time      */
    uint8_t    payload[JITTER_MAX_PAYLOAD];
    size_t     len;
} jitter_slot_t;
typedef struct {
    slot_state_t state;
    uint32_t     gen;
    uint64_t     ext_seq;
    uint32_t     rtp_ts;
    uint8_t      payload[JITTER_MAX_PAYLOAD];
    size_t       len;
} ring_slot_t;
typedef struct {
    ring_slot_t ring[JITTER_SLOTS];
    seqext_t    sx;
    int         frames_per_pkt;
    uint32_t    gen;
    uint64_t    play_cursor;    /* next ext_seq to pop            */
    uint64_t    boundary;       /* first accepted ext_seq (FLUSH) */
    int (*deadline_passed)(void *ud, uint64_t ext_seq);
    void *dl_ud;
    /* counters */
    unsigned dupes, late, lost, concealed_count, overwritten;
    unsigned resync_count;
    unsigned long resync_skipped;
} jitter_t;

void jitter_init(jitter_t*, int frames_per_pkt, uint16_t ref_seq, uint32_t ref_ts);
void jitter_set_deadline_passed(jitter_t*, int (*passed)(void*,uint64_t), void *ud);
int  jitter_insert(jitter_t*, uint16_t wire_seq, uint32_t wire_ts, const uint8_t*, size_t);
int  jitter_pop(jitter_t*, jitter_slot_t *out);
void jitter_flush(jitter_t*, uint16_t new_seq, uint32_t new_ts);
int  jitter_next_resend(jitter_t*, uint64_t *from, uint64_t *to);

/* Resync-to-live-edge safety net. If the play cursor has fallen far outside the
 * retained ring window (a transient sink/CPU/network stall lapped the buffer),
 * jump it forward to the start of a contiguous received run near the newest
 * received seq. Returns 1 (and sets *skipped = packets jumped) if it resynced,
 * else 0. Does NOT bump gen. Caller emits one silence period for the gap. */
int jitter_try_resync(jitter_t *j, uint64_t *skipped);
#endif
