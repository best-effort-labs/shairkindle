/* tests/test_session_flush.c — the generation-aware FLUSH barrier.
 *
 * Two parts, both over the same real-crypto / real-ALAC / fake-sink pipeline as
 * test_session_thread (loopback UDP, -DRAOP_TEST_SMALL_FILL):
 *
 *   Part A (live barrier) — start a session, play a couple frames, then
 *     session_flush(new_seq,new_ts) and feed a fresh contiguous run at the new
 *     boundary. Asserts the multithreaded INVARIANTS the brief permits us to keep
 *     deterministic: (b) session_current_gen incremented; (c) fresh decoded audio
 *     still flows after the barrier (not wedged); and session_stop joins cleanly
 *     with the flush machinery present (teardown ordering, no deadlock).
 *
 *   Part B (deterministic coherence unit) — assertion (d): the post-FLUSH RTP-ts
 *     discriminator the seq-only jitter boundary can't provide. Exercises the exact
 *     predicate rx_insert uses (session_reject_stale_audio): inert before any flush,
 *     and after a flush drops a packet whose rtptime is "before" boundary_ts under a
 *     WRAP-AWARE compare (a stale straggler whose 16-bit seq may re-extend above the
 *     new boundary) while admitting ts >= boundary.
 *
 * Assertion (a) from the brief ("only FRESH-packet PCM reaches the sink after the
 * barrier") is NOT content-checkable here: every fixture packet carries the same ALAC
 * frame, so stale and fresh decode to identical PCM. The deterministic fallback
 * we assert the live INVARIANTS above + the deterministic (d), and leave the PCM-tag
 * live race to an on-device/soak item. See the Task-5 report.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "bearssl_block.h"
#include "session.h"
#include "rtp.h"

#include "fixture_session_pcm.inc"

extern const uint8_t CT[];
extern const unsigned CT_LEN;

extern int      sink_rec_write_calls;
extern int16_t  sink_rec_buf[];
extern size_t   sink_rec_n;
extern size_t   sink_rec_max_accept;
extern int      sink_rec_flushed;

/* AES-CBC encrypt-mirror (same as test_session_thread): whole 16-byte blocks under (K,IV),
 * trailing len%16 bytes in the clear, IV copied before running. */
static void test_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                                 uint8_t *buf, size_t len) {
    size_t whole = len & ~(size_t)15;
    if (whole == 0) return;
    br_aes_big_cbcenc_keys ctx;
    br_aes_big_cbcenc_init(&ctx, key, 16);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    br_aes_big_cbcenc_run(&ctx, iv_copy, buf, whole);
}

/* Send one AES-encrypted ALAC audio datagram (seq,ts) from `tx` to `dst`. */
static void send_audio(int tx, const struct sockaddr_in *dst, const uint8_t *ciphertext,
                       uint16_t seq, uint32_t ts) {
    uint8_t pkt[12 + JITTER_MAX_PAYLOAD];
    pkt[0] = 0x80; pkt[1] = 0x60;
    pkt[2] = seq >> 8;  pkt[3] = seq & 0xff;
    pkt[4] = ts >> 24;  pkt[5] = ts >> 16;  pkt[6] = ts >> 8;  pkt[7] = ts & 0xff;
    pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;   /* SSRC */
    memcpy(pkt + 12, ciphertext, SLOT_FRAME_LEN);
    ssize_t sent = sendto(tx, pkt, 12 + SLOT_FRAME_LEN, 0,
                          (const struct sockaddr *)dst, sizeof *dst);
    assert(sent == (ssize_t)(12 + SLOT_FRAME_LEN));
}

/* Does a full SLOT_REF_S16 frame appear at some offset in [from, sink_rec_n)? */
static int frame_seen_from(size_t from) {
    if (sink_rec_n < SLOT_REF_N) return 0;
    for (size_t off = from; off + SLOT_REF_N <= sink_rec_n; off++)
        if (memcmp(sink_rec_buf + off, SLOT_REF_S16, SLOT_REF_N * sizeof(int16_t)) == 0)
            return 1;
    return 0;
}

int main(void) {
    static const uint8_t IV[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };

    raop_sdp_t sdp;
    memset(&sdp, 0, sizeof sdp);
    memcpy(sdp.rsaaeskey, CT, CT_LEN);
    sdp.rsaaeskey_len = CT_LEN;
    memcpy(sdp.aesiv, IV, 16);
    sdp.payload_type = 96;
    sdp.frame_length = SLOT_FRAME_SAMPLES;
    sdp.bit_depth    = SLOT_SAMPLE_SIZE;
    sdp.channels     = SLOT_CHANNELS;
    sdp.sample_rate  = SLOT_SAMPLE_RATE;
    memcpy(sdp.cookie, SLOT_COOKIE, SLOT_COOKIE_LEN);
    sdp.cookie_len = SLOT_COOKIE_LEN;

    session_t *s = session_create(&sdp, 70);
    assert(s != NULL);

    uint8_t key[16];
    memset(key, 'K', sizeof key);
    uint8_t ciphertext[JITTER_MAX_PAYLOAD];
    assert(SLOT_FRAME_LEN <= sizeof ciphertext);
    memcpy(ciphertext, SLOT_FRAME, SLOT_FRAME_LEN);
    test_aes_cbc_encrypt(key, IV, ciphertext, SLOT_FRAME_LEN);

    rtp_sockets_t socks;
    assert(rtp_open(&socks) == 0);

    /* Test-owned "sender" socket on loopback (source of audio, sink of outbound sends). */
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    assert(tx >= 0);
    struct sockaddr_in la;
    memset(&la, 0, sizeof la);
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = htons(0);
    assert(bind(tx, (struct sockaddr *)&la, sizeof la) == 0);
    socklen_t llen = sizeof la;
    assert(getsockname(tx, (struct sockaddr *)&la, &llen) == 0);
    uint16_t txport = ntohs(la.sin_port);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons((uint16_t)socks.server_port);

    sink_rec_write_calls = 0;
    sink_rec_n           = 0;
    sink_rec_max_accept  = (size_t)-1;

    const uint16_t anchor_seq = 100;
    const uint32_t anchor_ts  = 0;

    assert(session_start(s, &socks, anchor_seq, anchor_ts,
                         htonl(INADDR_LOOPBACK), txport, txport) == 0);

    /* ---- Part A: live barrier ------------------------------------------------ */

    /* Two in-order packets satisfy the (small) start-fill and begin playback. */
    for (int i = 0; i < 2; i++) {
        send_audio(tx, &dst, ciphertext, (uint16_t)(anchor_seq + i),
                   anchor_ts + (uint32_t)i * (uint32_t)SLOT_FRAME_SAMPLES);
        usleep(3000);
    }
    for (int w = 0; w < 4000 && sink_rec_n < SLOT_REF_N; w++) usleep(1000);
    assert(sink_rec_n >= SLOT_REF_N);                /* pre-flush audio is flowing */
    assert(session_current_gen(s) == 0);

    size_t base = sink_rec_n;                        /* frame-aligned high-water pre-flush */

    /* FLUSH to a new RTP-Info boundary well ahead of the pre-flush timestamps. */
    const uint16_t new_seq = anchor_seq + 50;        /* 150 */
    const uint32_t new_ts  = 200000;
    session_flush(s, new_seq, new_ts);

    assert(session_current_gen(s) == 1);             /* (b) gen bumped */

    /* sink_flush() runs off-lock on pl_thread: session_flush only sets
     * sink_restart_pending, and the player services it on its next loop iteration
     * (bounded by PL_WAIT_MS). Bounded-poll rather than asserting immediately -- that
     * would race the player thread's cycle and be flaky. */
    for (int w = 0; w < 4000 && !sink_rec_flushed; w++) usleep(1000);
    assert(sink_rec_flushed == 1);

    /* Feed a fresh contiguous run at the new boundary; every rtptime >= boundary_ts so
     * the coherence check admits them, and contiguity avoids any concealment. */
    for (int i = 0; i < 3; i++) {
        send_audio(tx, &dst, ciphertext, (uint16_t)(new_seq + i),
                   new_ts + (uint32_t)i * (uint32_t)SLOT_FRAME_SAMPLES);
        usleep(3000);
    }
    /* (c) not wedged: a real decoded frame reaches the sink AFTER the barrier. */
    for (int w = 0; w < 4000 && !frame_seen_from(base); w++) usleep(1000);
    assert(frame_seen_from(base));

    session_stop(s);        /* teardown ordering: joins cleanly with flush machinery present */

    /* ---- Part B: deterministic RTP-ts coherence (assertion d) ---------------- */

    /* Inert before any flush: a never-started/never-flushed session drops nothing. */
    session_t *s2 = session_create(&sdp, 70);
    assert(s2 != NULL);
    assert(session_reject_stale_audio(s2, 0) == 0);
    assert(session_reject_stale_audio(s2, 0xFFFFFFFFu) == 0);

    /* After the Part-A flush, boundary_ts == new_ts. A straggler one packet "before"
     * the boundary (its 16-bit seq could re-extend ABOVE the new seq-boundary, which the
     * jitter seq check would wrongly admit) is dropped; ts >= boundary is admitted. */
    assert(session_reject_stale_audio(s, new_ts - SLOT_FRAME_SAMPLES) == 1);  /* stale */
    assert(session_reject_stale_audio(s, new_ts) == 0);                       /* == boundary */
    assert(session_reject_stale_audio(s, new_ts + SLOT_FRAME_SAMPLES) == 0);  /* fresh */

    /* Wrap-aware: re-flush to a tiny boundary and probe a timestamp that is "before" it
     * only under the signed (int32) compare — an unsigned compare would wrongly admit it. */
    session_flush(s, 200, 5);
    assert(session_reject_stale_audio(s, 0xFFFFFFF0u) == 1);  /* -16 rel. => before  */
    assert(session_reject_stale_audio(s, 4)  == 1);           /* just before         */
    assert(session_reject_stale_audio(s, 5)  == 0);           /* == boundary         */
    assert(session_reject_stale_audio(s, 10) == 0);           /* after               */

    session_destroy(s2);
    session_destroy(s);
    close(tx);

    printf("test_session_flush OK\n");
    return 0;
}
