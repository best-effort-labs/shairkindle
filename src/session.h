/* src/session.h */
#ifndef SHAIRKINDLE_SESSION_H
#define SHAIRKINDLE_SESSION_H
#include <stdint.h>
#include <stddef.h>
#include "sdp.h"
#include "jitter.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct session session_t;

/* Allocate + configure a session from the negotiated SDP: OAEP-unwrap the 16-byte AES key,
   alac_open(cookie), sink_open(44100,vol). Returns NULL on failure (bad key / unsupported). */
session_t *session_create(const raop_sdp_t *sdp, int vol_pct);

/* Decrypt (fresh IV copy) + ALAC-decode one popped DATA slot into `pcm` (cap interleaved S16).
   Returns interleaved sample count, or -1. Uses the session key/iv/decoder. */
int  session_decode_slot(session_t *s, const jitter_slot_t *slot, int16_t *pcm, int cap);

/* Push `n` interleaved S16 samples to the sink, looping over partial accepts until fully
   committed, the session is torn down, or a sink error. Returns committed samples. `abort_gen`
   lets the caller interrupt on a FLUSH generation change (pass the gen sampled with the PCM;
   session_current_gen() compared each iteration). */
int  session_sink_write_all(session_t *s, const int16_t *pcm, int n, uint32_t abort_gen);

uint32_t session_current_gen(const session_t *s);

/* Total audio datagrams received on this session (lock-free stat for logging). */
unsigned long session_audio_pkts(const session_t *s);

/* Player-pipeline stats for bringup logging (all lock-free atomic snapshots). */
typedef struct {
    unsigned long recv;       /* audio datagrams received */
    unsigned long pops;       /* POP_DATA slots popped by the player */
    unsigned long decoded;    /* decodes returning >0 samples */
    unsigned long decfail;    /* decodes returning <=0 */
    long          sink_last;  /* last session_sink_write_all() return */
    int           started;    /* start-fill gate opened (player draining) */
    /* control-plane diagnostics (surfaced in the -f per-second stream line): */
    unsigned long sync;       /* CTRL_SYNC packets from sender */
    unsigned long treq;       /* timing requests we sent */
    unsigned long trep;       /* timing replies applied */
    unsigned long trej;       /* timing replies rejected (origin mismatch) */
    unsigned long resend;     /* resend requests we sent */
    long          off_ms;     /* last clock offset, ms (sanity) */
    /* jitter internals (diagnostic, unlocked reads): the seq-gap discriminator. */
    unsigned long play_cursor;/* next ext_seq to pop; FROZEN == permanent stall */
    long          gap_ahead;  /* dist play_cursor->nearest RECEIVED slot ahead, -1 none.
                                 > ~62 (the conceal window) == the stall-forever bug */
    unsigned      jdupe, jlate, jlost, jconceal, joverwr; /* jitter counters */
    /* audio-buffer growth discriminator: rate-error (steady climb) vs transient (flat). */
    long          sw_queued;  /* software PCM ring depth, stereo frames */
    long          drv_delay;  /* DMA/ALSA driver delay, frames (-1 n/a) */
    unsigned long mono_ms;    /* monotonic timestamp, ms -- real per-line dt */
    unsigned long resync;     /* jitter resync count */
    unsigned long skipped;    /* packets skipped by resync */
    int           respawns;   /* aplay respawns */
} session_stats_t;
void session_get_stats(const session_t *s, session_stats_t *out);

/* Generation-aware FLUSH barrier at a new RTP-Info boundary (seq,ts). Under the session lock:
   bump gen (player abandons stale in-flight PCM via the gen recheck in session_sink_write_all),
   jitter_flush(seq,ts) (invalidate old slots + rebase), record the ts boundary for the receiver
   coherence check, fire the async sink_flush(), broadcast the player onto the new epoch. Keeps
   the AES key + ALAC decoder alive (flush is a track-skip, not a teardown). */
void session_flush(session_t *s, uint16_t seq, uint32_t ts);

/* Post-FLUSH RTP-ts coherence predicate: 1 if this audio packet's RTP timestamp precedes the
   last flush boundary under a WRAP-AWARE compare — a stale pre-FLUSH straggler whose 16-bit seq
   may have re-extended above the new seq-boundary (which jitter's seq check can't catch), so it
   must be dropped, not inserted. Inert (0) before the first flush. The receiver calls it under
   s->lock; exposed for deterministic testing. */
int session_reject_stale_audio(const session_t *s, uint32_t rtptime);

/* Start the live session: take ownership of the bound rtp sockets, anchor the jitter buffer
   at (seq,ts), record the sender's addr + control/timing ports, spawn the receiver + player
   threads, begin the timing exchange. sender_ip_be is network byte order. Returns 0/-1.
   Ownership of `socks`' fds transfers to the session UNCONDITIONALLY, including on the -1
   failure return -- session_destroy() closes them. Callers must NOT also rtp_close(socks)
   (double-close); on failure, call session_destroy() to release the fds. */
int  session_start(session_t *s, const rtp_sockets_t *socks, uint16_t seq, uint32_t ts,
                   uint32_t sender_ip_be, uint16_t sender_control_port, uint16_t sender_timing_port);

/* Stop both threads (join). Idempotent; leaves the session allocated (destroy separately). */
void session_stop(session_t *s);

void session_destroy(session_t *s);   /* stop threads + alac_close + sink_close + free */

#ifdef __cplusplus
}
#endif

#endif
