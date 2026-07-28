/* tests/test_session_thread.c — session concurrency coverage.
 *
 * Drives the full receiver->jitter->player->fake-sink pipeline on loopback UDP with
 * REAL BearSSL crypto + REAL ALAC decode + the sink's record backend (SINK_RECORD_BACKEND):
 *   - session_create() from the OAEP fixture (key = 16x'K'), same as test_session_decode.
 *   - rtp_open() binds the 3 UDP sockets; session_start() takes ownership + spawns the
 *     receiver + player threads and anchors the jitter buffer at (anchor_seq, anchor_ts).
 *   - a test-owned "sender" UDP socket (bound to loopback) is the source of the audio
 *     datagrams (so the receiver's anti-spoof from-addr check passes) AND the harmless
 *     destination for the session's outbound resend/timing sends.
 *   - we sendto() a few in-order AES-encrypted ALAC audio packets and poll the fake sink
 *     until the first decoded frame arrives, then assert it byte-matches SLOT_REF_S16.
 *   - session_stop() must join both threads cleanly (bounded, no hang).
 *
 * Built with -DRAOP_TEST_SMALL_FILL so START_FILL_FRAMES is tiny (a couple packets start
 * playback) — otherwise the default ~2s fill would need hundreds of packets.
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

/* Same encrypt-mirror as test_session_decode.c: AES-CBC encrypt the plaintext ALAC frame
 * under (K, IV); trailing len%16 bytes stay in the clear; copy the IV before running. */
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
    sdp.payload_type  = 96;
    sdp.frame_length  = SLOT_FRAME_SAMPLES;
    sdp.bit_depth     = SLOT_SAMPLE_SIZE;
    sdp.channels      = SLOT_CHANNELS;
    sdp.sample_rate   = SLOT_SAMPLE_RATE;
    memcpy(sdp.cookie, SLOT_COOKIE, SLOT_COOKIE_LEN);
    sdp.cookie_len = SLOT_COOKIE_LEN;

    session_t *s = session_create(&sdp, 70);
    assert(s != NULL);

    /* Encrypt the fixture ALAC frame once — reused as every packet's payload. */
    uint8_t key[16];
    memset(key, 'K', sizeof key);
    uint8_t ciphertext[JITTER_MAX_PAYLOAD];
    assert(SLOT_FRAME_LEN <= sizeof ciphertext);
    memcpy(ciphertext, SLOT_FRAME, SLOT_FRAME_LEN);
    test_aes_cbc_encrypt(key, IV, ciphertext, SLOT_FRAME_LEN);

    /* Bind the session's 3 UDP sockets. */
    rtp_sockets_t socks;
    assert(rtp_open(&socks) == 0);

    /* Test-owned "sender" socket on loopback: source of audio, sink of outbound sends. */
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

    /* session takes ownership of socks' fds; sender addr = loopback, ctrl/timing ports = txport. */
    assert(session_start(s, &socks, anchor_seq, anchor_ts,
                         htonl(INADDR_LOOPBACK), txport, txport) == 0);

    /* Send a handful of in-order encrypted audio packets. */
    for (int i = 0; i < 6; i++) {
        uint8_t pkt[12 + JITTER_MAX_PAYLOAD];
        pkt[0] = 0x80;
        pkt[1] = 0x60;
        uint16_t seq = (uint16_t)(anchor_seq + i);
        uint32_t ts  = anchor_ts + (uint32_t)i * (uint32_t)SLOT_FRAME_SAMPLES;
        pkt[2] = seq >> 8;   pkt[3] = seq & 0xff;
        pkt[4] = ts >> 24;   pkt[5] = ts >> 16;   pkt[6] = ts >> 8;   pkt[7] = ts & 0xff;
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;   /* SSRC */
        memcpy(pkt + 12, ciphertext, SLOT_FRAME_LEN);
        ssize_t sent = sendto(tx, pkt, 12 + SLOT_FRAME_LEN, 0,
                              (struct sockaddr *)&dst, sizeof dst);
        assert(sent == (ssize_t)(12 + SLOT_FRAME_LEN));
        usleep(3000);
    }

    /* Poll (bounded ~4s) until the first full decoded frame has reached the sink. */
    for (int w = 0; w < 4000 && sink_rec_n < SLOT_REF_N; w++) usleep(1000);
    assert(sink_rec_n >= SLOT_REF_N);
    for (unsigned i = 0; i < SLOT_REF_N; i++) assert(sink_rec_buf[i] == SLOT_REF_S16[i]);

    /* --- lapped-buffer recovery ---
     * Wait for the player to fully drain the 6 in-order packets (play_cursor == 106),
     * then starve it with an unbridgeable gap (wire seqs 106..233 -- 128 packets, far
     * beyond the ~62-pkt conceal window this build's START_FILL_FRAMES/frame_length
     * gives) followed by a contiguous far-ahead run (234..241, 8 packets, >=
     * RESYNC_MIN_RUN). Without jitter_try_resync wired into pl_thread's stall branch,
     * play_cursor would freeze at 106 forever (the gap can't be concealed). With it,
     * the stall path detects the newest received seq (241) sits >= play_cursor+128 and
     * jumps play_cursor to run_start-PREROLL (~218) in one jitter_pop cycle -- past the
     * 62-pkt conceal window, which pure draining/concealment could never reach. */
    session_stats_t st;
    for (int w = 0; w < 4000; w++) {
        session_get_stats(s, &st);
        if (st.play_cursor >= (unsigned long)(anchor_seq + 6)) break;
        usleep(1000);
    }
    assert(st.play_cursor >= (unsigned long)(anchor_seq + 6));
    unsigned long a = st.play_cursor;

    for (int i = 0; i < 8; i++) {
        uint8_t pkt[12 + JITTER_MAX_PAYLOAD];
        pkt[0] = 0x80;
        pkt[1] = 0x60;
        uint16_t seq = (uint16_t)(anchor_seq + 134 + i);   /* 234..241: past a+128 */
        uint32_t ts  = anchor_ts + (uint32_t)(134 + i) * (uint32_t)SLOT_FRAME_SAMPLES;
        pkt[2] = seq >> 8;   pkt[3] = seq & 0xff;
        pkt[4] = ts >> 24;   pkt[5] = ts >> 16;   pkt[6] = ts >> 8;   pkt[7] = ts & 0xff;
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;   /* SSRC */
        memcpy(pkt + 12, ciphertext, SLOT_FRAME_LEN);
        ssize_t sent = sendto(tx, pkt, 12 + SLOT_FRAME_LEN, 0,
                              (struct sockaddr *)&dst, sizeof dst);
        assert(sent == (ssize_t)(12 + SLOT_FRAME_LEN));
        usleep(1000);
    }

    unsigned long b = a;
    for (int w = 0; w < 2000; w++) {   /* bounded ~2s poll, well past PL_WAIT_MS cycles */
        session_get_stats(s, &st);
        b = st.play_cursor;
        if (b > a + 62) break;
        usleep(1000);
    }
    assert(b > a + 62);    /* past the conceal window -- only resync gets here */

    /* --- Fix-1 regression: a SECOND unbridgeable gap must ALSO recover ---
     * The first recovery just popped 24 slots (16 concealed preroll + 8 real data,
     * ext_seq 218..241) to reach play_cursor==b(~242). Pre-fix, resync_hold only
     * decremented on POP_DATA, so those 8 data pops left it at 64-8=56 (still >0).
     * This second gap is a PURE stall -- no data exists at all until the far run
     * below arrives -- so under the pre-fix code resync_hold can NEVER decrement
     * further (nothing pops), the `resync_hold <= 0` gate stays false forever, and
     * jitter_try_resync is never even called: a deterministic permanent wedge, not
     * a timing race. The per-iteration decrement (top of pl_thread's loop, this
     * fix) drains resync_hold to 0 regardless of the stall, re-arming resync so
     * this second lap recovers too. */
    for (int i = 0; i < 8; i++) {
        uint8_t pkt[12 + JITTER_MAX_PAYLOAD];
        pkt[0] = 0x80;
        pkt[1] = 0x60;
        uint16_t seq = (uint16_t)(anchor_seq + 275 + i);   /* far past b(~242)+128 */
        uint32_t ts  = anchor_ts + (uint32_t)(275 + i) * (uint32_t)SLOT_FRAME_SAMPLES;
        pkt[2] = seq >> 8;   pkt[3] = seq & 0xff;
        pkt[4] = ts >> 24;   pkt[5] = ts >> 16;   pkt[6] = ts >> 8;   pkt[7] = ts & 0xff;
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;   /* SSRC */
        memcpy(pkt + 12, ciphertext, SLOT_FRAME_LEN);
        ssize_t sent = sendto(tx, pkt, 12 + SLOT_FRAME_LEN, 0,
                              (struct sockaddr *)&dst, sizeof dst);
        assert(sent == (ssize_t)(12 + SLOT_FRAME_LEN));
        usleep(1000);
    }

    unsigned long c = b;
    for (int w = 0; w < 3000; w++) {   /* bounded ~3s poll */
        session_get_stats(s, &st);
        c = st.play_cursor;
        if (c > b + 62) break;
        usleep(1000);
    }
    assert(c > b + 62);    /* second lap recovered too -- resync re-armed, not wedged */

    session_stop(s);       /* must join both threads — no hang */
    session_destroy(s);
    close(tx);

    printf("test_session_thread OK\n");
    return 0;
}
