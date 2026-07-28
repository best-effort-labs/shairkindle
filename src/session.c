/* src/session.c — session core and the two live worker threads.
 *
 * Concurrency contract: jitter_t and raop_clock_t have NO internal
 * locking, so EVERY jitter_ and clock_ call runs under
 * session->lock, held ONLY for that short op -- NEVER across session_decode_slot /
 * session_sink_write_all (the player pops-and-unlocks, then does the slow decode+sink work
 * outside the lock so it never blocks the receiver from inserting). running/gen are
 * _Atomic. When jitter_pop stalls the player unlocks and polls (nanosleep PL_WAIT_MS)
 * rather than waiting on a condvar -- a deliberate choice: pthread_cond_timedwait pulls
 * in clock_gettime, which crashes on this pre-vDSO ARM kernel (see mono_ns below).
 * Teardown sets running=0; both threads observe it within one poll/nanosleep cycle
 * and join cleanly (the receiver's poll timeout bounds the same for it).
 */
#include "session.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/times.h>
#include <netinet/in.h>
#include "raop_crypto.h"
#include "alac_shim.h"
#include "sink.h"
#include "raop_clock.h"

/* Startup buffer fill before the player begins draining (spec Latency policy). The thread
 * test overrides this tiny so a couple packets suffice. */
#ifdef RAOP_TEST_SMALL_FILL
# define START_FILL_FRAMES 8192    /* latency knob, tune on real K3 */
#else
# define START_FILL_FRAMES 22050   /* 0.5s @44.1k. MUST stay below the sender's initial
                                     * burst (~140 pkts/~1.1s) or the fill gate never opens:
                                     * the sender waits for playback to start before sending
                                     * more, so 88200 (2s) deadlocked real senders on the K3.
                                     * If you lower this: keep START_FILL_FRAMES/frames_per_pkt
                                     * > RESYNC_PREROLL (jitter.c; currently 62 > 16), or the
                                     * post-resync preroll stalls instead of concealing and
                                     * permanently wedges playback -- see RESYNC_PREROLL. */
#endif

#define RX_POLL_MS      20         /* receiver poll timeout -> teardown latency bound   */
#define PL_WAIT_MS      20         /* player stall poll interval (nanosleep bound)      */
#define PERIODIC_MS     200        /* resend scan + timing-request cadence              */
#define TIMING_TIMEOUT_NS 3000000000ULL  /* timing-reply timeout, tune on real K3 */
#define RX_DRAIN_BUDGET 64         /* per-call datagram cap -> bounds teardown under a flood */

struct session {
    uint8_t   key[16];
    uint8_t   iv[16];
    alac_dec *dec;
    int       frames_per_pkt;
    _Atomic uint32_t gen;

    /* live threads + shared state */
    pthread_t       rx, pl;
    jitter_t        jit;
    raop_clock_t    clk;
    int             audio_fd, control_fd, timing_fd;
    uint32_t        sender_ip;     /* network byte order */
    uint16_t        sctrl, stim;   /* sender's control/timing ports */
    _Atomic int     running;
    _Atomic int     sink_restart_pending;   /* set by session_flush, serviced by pl_thread */
    _Atomic unsigned long audio_pkts;  /* audio datagrams received (lock-free stat) */
    _Atomic unsigned long dbg_pops;    /* POP_DATA slots the player popped */
    _Atomic unsigned long dbg_decoded; /* decodes that returned >0 samples */
    _Atomic unsigned long dbg_decfail; /* decodes that returned <=0 */
    _Atomic long          dbg_sinklast;/* last session_sink_write_all() return */
    _Atomic int           dbg_started; /* the start-fill gate has opened */
    /* control-plane diagnostics (surfaced in the -f per-second stream line): */
    _Atomic unsigned long dbg_sync, dbg_treq, dbg_trep, dbg_trej, dbg_resend;
    _Atomic long          dbg_off_ms;  /* last clock offset in ms */
    _Atomic int     threads_up;     /* written by session_start/session_stop, read by pl_thread's
                                        teardown guard in session_sink_write_all -- cross-thread,
                                        must be atomic (mirrors running/gen/sink_restart_pending) */
    pthread_mutex_t lock;
    uint64_t        timing_t1_ns;
    uint64_t        timing_token_ntp;  /* the NTP token placed in the outstanding request's tx field */
    int             timing_outstanding;
    long            rx_count;       /* successful inserts (fill gate); under lock */
    int             started;        /* player has passed the start-fill gate; under lock */
    uint32_t        sync_rtp;       /* last SYNC anchor (recorded; playback-timing use deferred) */
    uint64_t        sync_ntp;
    int16_t        *pcm;            /* player decode/silence scratch */
    int             pcm_cap;
    uint32_t        boundary_ts;    /* RTP ts of the last FLUSH boundary; under lock        */
    int             flushed;        /* a FLUSH has occurred (arms the ts coherence check)   */
    int             resync_hold;    /* iterations remaining before another resync is allowed;
                                        only touched by pl_thread, so no atomic needed. Decremented
                                        ONCE PER pl_thread LOOP ITERATION regardless of outcome
                                        (data pop, conceal pop, or stall) -- see pl_thread. A stall
                                        storm (no POP_DATA at all) must still drain this to 0, or a
                                        second buffer lap inside the hold window would gate resync
                                        off forever (the exact wedge this hysteresis exists to
                                        prevent).                                              */
};

/* ---- small clocks -------------------------------------------------------- */
/* Monotonic nanoseconds via times() -- a plain syscall with NO vDSO probe.
 *
 * We must AVOID libc clock_gettime on the device: zig's musl runs a one-time vDSO
 * detection (cgt_init) on the first clock_gettime, which faults on this pre-vDSO ARM
 * kernel (2.6.26) -> layout-dependent SIGSEGV (a heisenbug); the raw syscall dodges the
 * crash but leaves ts unfilled. times() is monotonic (ticks since boot), needs no vDSO,
 * and marshals correctly -- same fix as the supervisor's now_ms() (src/supervisor_main.c
 * has the full write-up). Resolution is one tick (~10ms at 100Hz), fine here: every
 * consumer is a delta -- periodic pacing (200ms) and the timing-reply RTT, which only
 * feeds the clock offset, a diagnostic stat (playback is buffer-driven, not clock-scheduled).
 * Dropping the condvar's timedwait for a relative nanosleep (see pl_thread) removes the
 * last clock_gettime, so no crashing path is reachable from raopd.
 * clock_t wraps ~248d uptime at 100Hz; a Kindle reboots/sleeps far sooner. */
static uint64_t mono_ns(void) {
    static long hz = 0;
    if (hz <= 0) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    struct tms tb;
    clock_t t = times(&tb);
    return (uint64_t)t * 1000000000ull / (uint64_t)hz;
}

/* Deadline predicate (clock-light first cut): a missing slot is past deadline once the
 * buffer has advanced START_FILL_FRAMES worth of packets beyond it — i.e. we already hold
 * a packet that far ahead, so waiting longer is pointless; conceal instead of stalling.
 * Called from jitter_pop, which the player runs under s->lock, so reading j->ring is safe. */
static int session_deadline(void *ud, uint64_t ext_seq) {
    session_t *s = (session_t *)ud;
    jitter_t *j = &s->jit;
    int npkts = START_FILL_FRAMES / (j->frames_per_pkt > 0 ? j->frames_per_pkt : 1);
    if (npkts < 1) npkts = 1;
    uint64_t last = ext_seq + (uint64_t)npkts;
    for (uint64_t e = ext_seq + 1; e <= last && e < ext_seq + JITTER_SLOTS; e++) {
        ring_slot_t *rs = &j->ring[e % JITTER_SLOTS];
        if (rs->gen == j->gen && rs->ext_seq == e && rs->state == SLOT_RECEIVED) return 1;
    }
    return 0;
}

/* Send one timing request (records t1 = monotonic now, marks it outstanding). Only ever
 * one outstanding at a time so the t1<->reply correlation is unambiguous. Only marks
 * outstanding on a SUCCESSFUL build+send -- a failed build or sendto must NOT set
 * outstanding, or a dropped/failed send would wedge the exchange (nothing would ever
 * clear it, since only a parsed reply or the timeout does). */
static void send_timing_request(session_t *s) {
    uint64_t t1 = mono_ns();
    pthread_mutex_lock(&s->lock);
    int64_t off = s->clk.offset_ns;
    pthread_mutex_unlock(&s->lock);

    uint64_t tok = ntp_from_ns(t1, off);
    uint8_t req[32];
    if (rtp_build_timing_request(tok, req, sizeof req) != 32) return;
    if (rtp_sendto(s->timing_fd, req, 32, s->sender_ip, s->stim) < 0) return;

    pthread_mutex_lock(&s->lock);
    s->timing_t1_ns = t1;
    s->timing_token_ntp = tok;
    s->timing_outstanding = 1;
    pthread_mutex_unlock(&s->lock);
    s->dbg_treq++;
}

/* ---- receiver thread ----------------------------------------------------- */
/* Post-FLUSH coherence: jitter_insert only checks the (16-bit-rebased) seq boundary, so a
 * delayed pre-FLUSH datagram whose seq re-extends above the new boundary would be wrongly
 * admitted as new-epoch audio. The RTP-ts is the discriminator the seq check lacks: drop any
 * packet "before" boundary_ts under a wrap-aware (signed) compare. Inert until the first flush.
 * Caller holds s->lock (or the session is single-threaded, e.g. a test). */
int session_reject_stale_audio(const session_t *s, uint32_t rtptime) {
    if (!s || !s->flushed) return 0;
    return (int32_t)(rtptime - s->boundary_ts) < 0;
}

static void rx_insert(session_t *s, const rtp_audio_t *a) {
    s->audio_pkts++;                       /* atomic; lock-free stat for the foreground log */
    pthread_mutex_lock(&s->lock);
    if (session_reject_stale_audio(s, a->rtptime)) {   /* stale pre-FLUSH straggler; drop */
        pthread_mutex_unlock(&s->lock);
        return;
    }
    int r = jitter_insert(&s->jit, a->seq, a->rtptime, a->payload, a->payload_len);
    if (r == 0) s->rx_count++;   /* player polls rx_count/jitter; no wakeup needed */
    pthread_mutex_unlock(&s->lock);
}

static void rx_dispatch(session_t *s, int fd, const uint8_t *buf, size_t n) {
    if (fd == s->audio_fd) {
        rtp_audio_t a;
        if (rtp_parse_audio(buf, n, &a) == 0) rx_insert(s, &a);
        return;
    }
    if (fd == s->control_fd) {
        const uint8_t *inner = NULL; size_t ilen = 0;
        ctrl_kind_t k = rtp_classify_control(buf, n, &inner, &ilen);
        if (k == CTRL_SYNC) {
            s->dbg_sync++;
            uint32_t rtp_now; uint64_t ntp_now;
            if (rtp_parse_sync(buf, n, &rtp_now, &ntp_now) == 0) {
                pthread_mutex_lock(&s->lock);
                s->sync_rtp = rtp_now;
                s->sync_ntp = ntp_now;
                pthread_mutex_unlock(&s->lock);
            }
        } else if (k == CTRL_RESEND_AUDIO) {
            rtp_audio_t a;
            if (rtp_parse_audio(inner, ilen, &a) == 0) rx_insert(s, &a);
        }
        return;
    }
    if (fd == s->timing_fd) {
        pthread_mutex_lock(&s->lock);
        int outstanding = s->timing_outstanding;
        uint64_t t1 = s->timing_t1_ns;
        uint64_t token = s->timing_token_ntp;
        pthread_mutex_unlock(&s->lock);
        if (!outstanding) return;
        uint64_t remote_ntp, origin_echo;
        if (rtp_parse_timing_reply(buf, n, &remote_ntp, &origin_echo) == 0) {
            if (origin_echo != token) { s->dbg_trej++; return; }  /* stale/spoofed reply -- drop, keep waiting */
            uint64_t t4 = mono_ns();
            pthread_mutex_lock(&s->lock);
            clock_timing_reply(&s->clk, t1, remote_ntp, t4);
            int64_t off = s->clk.offset_ns;
            s->timing_outstanding = 0;
            pthread_mutex_unlock(&s->lock);
            s->dbg_trep++;
            s->dbg_off_ms = (long)(off / 1000000);
        }
    }
}

/* Bounded by BOTH a per-call datagram budget and a running check: a flooding peer keeps
 * rtp_recv() returning >0 indefinitely, and without a bound this loop would never return,
 * so rx_thread would never re-poll s->running and session_stop's pthread_join would hang.
 * rx_thread re-polls immediately when the socket stays readable, so the budget costs no
 * throughput -- it just gives teardown a chance to win between budgets. */
static void rx_drain(session_t *s, int fd) {
    uint8_t buf[JITTER_MAX_PAYLOAD + 64];
    for (int i = 0; i < RX_DRAIN_BUDGET && s->running; i++) {
        struct sockaddr_in from;
        memset(&from, 0, sizeof from);
        ssize_t n = rtp_recv(fd, buf, sizeof buf, &from);
        if (n <= 0) break;                              /* 0 = would-block, <0 = error */
        if (from.sin_addr.s_addr != s->sender_ip) continue;  /* anti-spoof: drop mismatched source */
        rx_dispatch(s, fd, buf, (size_t)n);
    }
}

static void *rx_thread(void *ud) {
    session_t *s = (session_t *)ud;
    uint64_t last_periodic = mono_ns();
    while (s->running) {
        struct pollfd fds[3] = {
            { s->audio_fd,   POLLIN, 0 },
            { s->control_fd, POLLIN, 0 },
            { s->timing_fd,  POLLIN, 0 },
        };
        int pr = poll(fds, 3, RX_POLL_MS);
        if (pr > 0) {
            if (fds[0].revents & POLLIN) rx_drain(s, s->audio_fd);
            if (fds[1].revents & POLLIN) rx_drain(s, s->control_fd);
            if (fds[2].revents & POLLIN) rx_drain(s, s->timing_fd);
        }

        uint64_t now = mono_ns();
        if (now - last_periodic >= (uint64_t)PERIODIC_MS * 1000000ull) {
            last_periodic = now;

            /* resend: coalesced missing run, ext_seq [from,to] inclusive -> wire first+count */
            uint64_t from, to;
            pthread_mutex_lock(&s->lock);
            int gap = (jitter_next_resend(&s->jit, &from, &to) == 0);
            pthread_mutex_unlock(&s->lock);
            if (gap) {
                uint16_t first = (uint16_t)from;
                uint16_t count = (uint16_t)(to - from + 1);
                uint8_t r[8];
                if (rtp_build_resend(first, count, r, sizeof r) == 8) {
                    rtp_sendto(s->control_fd, r, 8, s->sender_ip, s->sctrl);
                    s->dbg_resend++;
                }
            }

            /* one fresh timing request if none outstanding; a reply that never arrives
             * (dropped UDP) is recovered by the timeout below rather than wedging forever */
            pthread_mutex_lock(&s->lock);
            if (s->timing_outstanding && (now - s->timing_t1_ns) > TIMING_TIMEOUT_NS)
                s->timing_outstanding = 0;
            int outstanding = s->timing_outstanding;
            pthread_mutex_unlock(&s->lock);
            if (!outstanding) send_timing_request(s);
        }
    }
    return NULL;
}

/* ---- player thread ------------------------------------------------------- */
static void *pl_thread(void *ud) {
    session_t *s = (session_t *)ud;
    while (s->running) {
        /* service a pending FLUSH restart off-lock: the pipe/dmix restart blocks
         * ~100ms via reap+spawn, so it must run before we ever take s->lock. */
        if (atomic_exchange(&s->sink_restart_pending, 0)) sink_flush();

        /* Resync hysteresis re-arm: decrement once per loop iteration no matter what this
         * iteration did (data pop, conceal pop, or stall). pl_thread-only, so no lock/atomic
         * needed. Must NOT also decrement in the POP_DATA branch below -- that would double-
         * decrement the fast-playback case while leaving a stall-only storm (rc==1 every
         * iteration, no POP_DATA ever) stuck at whatever value resync last set, which is the
         * permanent-wedge bug this hysteresis is supposed to prevent. */
        if (s->resync_hold > 0) s->resync_hold--;

        pthread_mutex_lock(&s->lock);

        if (!s->started) {
            if ((long)s->rx_count * s->frames_per_pkt >= START_FILL_FRAMES) {
                s->started = 1;
                s->dbg_started = 1;
            } else {
                pthread_mutex_unlock(&s->lock);
                struct timespec w = { 0, PL_WAIT_MS * 1000000L };
                nanosleep(&w, NULL);   /* poll for fill; no cond -> no clock_gettime */
                continue;
            }
        }

        jitter_slot_t slot;
        int rc = jitter_pop(&s->jit, &slot);
        uint32_t g = s->gen;
        if (rc == 1) {                        /* stall: no data -- try a resync before polling */
            uint64_t skipped = 0;
            int did = (s->resync_hold <= 0) && jitter_try_resync(&s->jit, &skipped);
            if (did) s->resync_hold = 64;      /* hysteresis: normal pops before next resync */
            pthread_mutex_unlock(&s->lock);
            if (did) {                         /* one silence period for the discontinuity */
                int n = s->frames_per_pkt * 2;
                if (n > s->pcm_cap) n = s->pcm_cap;
                memset(s->pcm, 0, (size_t)n * sizeof(int16_t));
                session_sink_write_all(s, s->pcm, n, g);
            } else {
                struct timespec w = { 0, PL_WAIT_MS * 1000000L };
                nanosleep(&w, NULL);
            }
            continue;
        }
        pthread_mutex_unlock(&s->lock);       /* decode + sink strictly OUTSIDE the lock */

        if (slot.kind == POP_DATA) {
            s->dbg_pops++;
            int n = session_decode_slot(s, &slot, s->pcm, s->pcm_cap);
            if (n > 0) { s->dbg_decoded++;
                s->dbg_sinklast = session_sink_write_all(s, s->pcm, n, g); }
            else s->dbg_decfail++;
        } else {                              /* POP_CONCEAL: one packet of silence */
            int n = s->frames_per_pkt * 2;
            if (n > s->pcm_cap) n = s->pcm_cap;
            memset(s->pcm, 0, (size_t)n * sizeof(int16_t));
            session_sink_write_all(s, s->pcm, n, g);
        }
    }
    return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */
session_t *session_create(const raop_sdp_t *sdp, int vol_pct) {
    if (!sdp) return NULL;
    uint8_t key[16];
    if (!raop_oaep_unwrap_key(sdp->rsaaeskey, sdp->rsaaeskey_len, key)) return NULL;

    alac_dec *dec = alac_open(sdp->frame_length, 16, sdp->channels,
                              sdp->sample_rate, sdp->cookie, sdp->cookie_len);
    if (!dec) return NULL;

    if (sink_open(sdp->sample_rate, vol_pct) != 0) {
        alac_close(dec);
        return NULL;
    }

    session_t *s = (session_t *)calloc(1, sizeof *s);
    if (!s) {
        alac_close(dec);
        sink_close();
        return NULL;
    }
    memcpy(s->key, key, 16);
    memcpy(s->iv, sdp->aesiv, 16);
    s->dec = dec;
    s->frames_per_pkt = sdp->frame_length;
    s->gen = 0;
    s->audio_fd = s->control_fd = s->timing_fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

int session_decode_slot(session_t *s, const jitter_slot_t *slot, int16_t *pcm, int cap) {
    if (!s || !slot) return -1;
    if (slot->len == 0 || slot->len > JITTER_MAX_PAYLOAD) return -1;

    uint8_t buf[JITTER_MAX_PAYLOAD];
    memcpy(buf, slot->payload, slot->len);   /* decrypt is in-place -- work on a copy */

    uint8_t iv_local[16];
    memcpy(iv_local, s->iv, 16);             /* don't mutate the stored per-session IV */

    raop_aes_cbc_decrypt(s->key, iv_local, buf, slot->len);

    return alac_decode(s->dec, buf, slot->len, pcm, cap);
}

int session_sink_write_all(session_t *s, const int16_t *pcm, int n, uint32_t abort_gen) {
    if (!s || n <= 0) return 0;

    int off = 0;
    int backoff_retries = 0;
    const int MAX_BACKOFF_RETRIES = 200;   /* bounded: never spin forever on a stuck sink */

    while (off < n) {
        if (session_current_gen(s) != abort_gen) break;   /* FLUSH happened mid-write */
        /* threads_up gates this: running==0 also holds for a session that was created
         * but never session_start()'d (unit tests call session_sink_write_all directly
         * in that state and must still loop to completion) -- threads_up&&!running is
         * the real-teardown signal, matching session_stop()'s own check. */
        if (s->threads_up && !s->running) break;          /* teardown mid-write -- don't wait out the backoff */

        int accepted = sink_write(pcm + off, n - off);
        if (accepted < 0) break;                          /* sink error */
        if (accepted == 0) {
            if (++backoff_retries > MAX_BACKOFF_RETRIES) break;
            usleep(1000);
            continue;
        }
        backoff_retries = 0;
        off += accepted;
    }
    return off;
}

uint32_t session_current_gen(const session_t *s) {
    return s ? s->gen : 0;
}

unsigned long session_audio_pkts(const session_t *s) {
    return s ? s->audio_pkts : 0;
}

void session_get_stats(const session_t *s, session_stats_t *out) {
    if (!out) return;
    if (!s) { memset(out, 0, sizeof *out); return; }
    out->recv      = s->audio_pkts;
    out->pops      = s->dbg_pops;
    out->decoded   = s->dbg_decoded;
    out->decfail   = s->dbg_decfail;
    out->sink_last = s->dbg_sinklast;
    out->started   = s->dbg_started;
    out->sync      = s->dbg_sync;
    out->treq      = s->dbg_treq;
    out->trep      = s->dbg_trep;
    out->trej      = s->dbg_trej;
    out->resend    = s->dbg_resend;
    out->off_ms    = s->dbg_off_ms;
    /* jitter internals: diagnostic snapshot, unlocked (a torn count is harmless in a
     * bringup log). gap_ahead scans the ring for the nearest RECEIVED slot ahead of the
     * play cursor -- the direct measure of "is the sender past the 62-pkt conceal window". */
    const jitter_t *j = &s->jit;
    out->play_cursor = (unsigned long)j->play_cursor;
    out->jdupe   = j->dupes;
    out->jlate   = j->late;
    out->jlost   = j->lost;
    out->jconceal= j->concealed_count;
    out->joverwr = j->overwritten;
    out->resync   = j->resync_count;
    out->skipped  = j->resync_skipped;
    out->respawns = sink_respawns();
    long gap = -1;
    for (uint64_t k = 1; k < JITTER_SLOTS; k++) {
        uint64_t e = j->play_cursor + k;
        const ring_slot_t *rs = &j->ring[e % JITTER_SLOTS];
        if (rs->gen == j->gen && rs->ext_seq == e && rs->state == SLOT_RECEIVED) { gap = (long)k; break; }
    }
    out->gap_ahead = gap;
    out->sw_queued = sink_queued_frames();
    out->drv_delay = sink_driver_delay_frames();
    out->mono_ms   = (unsigned long)(mono_ns() / 1000000ull);
}

/* The 7-step FLUSH barrier. Whole body under s->lock, held only for
 * the short bookkeeping — never blocks: step (6) just sets an atomic flag here; pl_thread
 * services it off-lock (calling the real sink_flush(), which blocks ~100ms via reap+spawn
 * for the aplay/dmix sink) on its next loop iteration, so a stop racing the flush just waits
 * briefly for the lock, no deadlock. */
void session_flush(session_t *s, uint16_t seq, uint32_t ts) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    /* (1) barrier entry: whole body runs under s->lock */
    s->gen++;                                 /* (2) atomic; player drops old-gen in-flight PCM */
    jitter_flush(&s->jit, seq, ts);           /* (3) invalidate old slots + rebase seqext/boundary */
    s->boundary_ts = ts;                      /* (4) the receiver's ts coherence check needs this */
    s->flushed = 1;
    s->started = 0;                           /*     re-arm the start-fill gate for the new epoch */
    s->rx_count = 0;                          /*     drop stale pre-FLUSH inserts from the count  */
    /* (5) no extra action: the player rechecks s->gen in session_sink_write_all and abandons
     *     any old-gen remainder on its own. */
    /* (6) async restart: the player thread services this off-lock (a pipe/dmix restart
     *     must not run under session->lock). */
    atomic_store(&s->sink_restart_pending, 1);
    /* (7) barrier exit; the player picks up the new epoch on its next poll */
    pthread_mutex_unlock(&s->lock);
}

/* Ownership of socks' fds transfers to `s` unconditionally, even on the -1 failure return
 * below (s->audio_fd/control_fd/timing_fd are set before any failure path) -- the caller
 * must clean up via session_destroy(), never rtp_close(socks) (would double-close). */
int session_start(session_t *s, const rtp_sockets_t *socks, uint16_t seq, uint32_t ts,
                  uint32_t sender_ip_be, uint16_t sender_control_port, uint16_t sender_timing_port) {
    if (!s || !socks) return -1;

    s->audio_fd   = socks->audio_fd;
    s->control_fd = socks->control_fd;
    s->timing_fd  = socks->timing_fd;
    s->sender_ip  = sender_ip_be;
    s->sctrl      = sender_control_port;
    s->stim       = sender_timing_port;

    s->pcm_cap = s->frames_per_pkt * 2;      /* stereo interleaved, one max frame */
    s->pcm = (int16_t *)malloc((size_t)s->pcm_cap * sizeof(int16_t));
    if (!s->pcm) return -1;

    jitter_init(&s->jit, s->frames_per_pkt, seq, ts);
    jitter_set_deadline_passed(&s->jit, session_deadline, s);
    clock_init(&s->clk);

    s->rx_count = 0;
    s->started = 0;
    s->timing_outstanding = 0;
    s->timing_t1_ns = 0;
    s->timing_token_ntp = 0;
    s->sync_rtp = 0;
    s->sync_ntp = 0;
    s->running = 1;

    send_timing_request(s);                  /* begin the timing exchange */

    if (pthread_create(&s->rx, NULL, rx_thread, s) != 0) {
        s->running = 0; free(s->pcm); s->pcm = NULL; return -1;
    }
    if (pthread_create(&s->pl, NULL, pl_thread, s) != 0) {
        s->running = 0;
        pthread_join(s->rx, NULL);
        free(s->pcm); s->pcm = NULL;
        return -1;
    }
    s->threads_up = 1;
    return 0;
}

void session_stop(session_t *s) {
    if (!s || !s->threads_up) return;
    s->running = 0;                          /* both threads observe this within ~PL_WAIT_MS/RX_POLL_MS */
    pthread_join(s->rx, NULL);
    pthread_join(s->pl, NULL);
    s->threads_up = 0;
}

void session_destroy(session_t *s) {
    if (!s) return;
    session_stop(s);                         /* idempotent; safe if never started */
    if (s->audio_fd   >= 0) close(s->audio_fd);
    if (s->control_fd >= 0) close(s->control_fd);
    if (s->timing_fd  >= 0) close(s->timing_fd);
    free(s->pcm);
    pthread_mutex_destroy(&s->lock);
    alac_close(s->dec);
    sink_close();
    free(s);
}
