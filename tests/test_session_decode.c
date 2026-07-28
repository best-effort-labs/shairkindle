/* tests/test_session_decode.c — session core, host-tested with real BearSSL
 * crypto + REAL ALAC decode + the sink's record backend (src/sink.cpp, SINK_RECORD_BACKEND).
 *
 * There is no test-only key hook: session_create() always OAEP-unwraps sdp->rsaaeskey.
 * So this exercises the true unwrap->decrypt->decode chain end to end:
 *   - sdp.rsaaeskey  = the foundation OAEP fixture's CT (tests/fixture_oaep.c),
 *     which raop_oaep_unwrap_key() recovers to the known 16-byte key "KKKK...K".
 *   - sdp.cookie/frame_length = tests/fixture_session_pcm.inc's small (RAOP-realistic)
 *     ALAC fixture -- see that file for why it's a fresh fixture rather than
 *     tests/fixture_alac.c (that one's 3364-byte frame doesn't fit a jitter_slot_t's
 *     2048-byte payload array).
 *   - The slot ciphertext is made here by AES-CBC *encrypting* the fixture's plain
 *     ALAC frame under (K, IV) with a tiny local mirror of src/aes_cbc.c's decrypt
 *     (same whole = len & ~15 trailing-bytes-in-clear handling, inverse direction).
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bearssl_block.h"
#include "session.h"

#include "fixture_session_pcm.inc"

extern const uint8_t CT[];
extern const unsigned CT_LEN;

extern size_t   sink_rec_max_accept;
extern int      sink_rec_write_calls;
extern int16_t  sink_rec_buf[];
extern size_t   sink_rec_n;

/* Mirrors src/aes_cbc.c's raop_aes_cbc_decrypt EXACTLY, but the encrypt direction:
 * same "whole = len & ~15" handling (trailing len%16 bytes stay in the clear), same
 * "copy the IV before running" (br_aes_big_cbcenc_run mutates it in place). */
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

    /* session_create succeeding at all proves the OAEP unwrap of the fixture
     * rsaaeskey worked (it's the only path to a valid key -> valid alac_open). */
    session_t *s = session_create(&sdp, 70);
    assert(s != NULL);
    /* open-arg-forwarding (rate/vol into sink_open) isn't checkable here: the
     * record backend's sink_open(int,int) ignores its args by design. */

    /* Build the ciphertext slot payload: encrypt the plaintext ALAC frame under the
     * known key K="KKKKKKKKKKKKKKKK" (what CT unwraps to) + our chosen IV. */
    uint8_t key[16];
    memset(key, 'K', sizeof key);
    uint8_t ciphertext[JITTER_MAX_PAYLOAD];
    assert(SLOT_FRAME_LEN <= sizeof ciphertext);
    memcpy(ciphertext, SLOT_FRAME, SLOT_FRAME_LEN);
    test_aes_cbc_encrypt(key, IV, ciphertext, SLOT_FRAME_LEN);

    jitter_slot_t slot;
    memset(&slot, 0, sizeof slot);
    slot.kind    = POP_DATA;
    slot.ext_seq = 0;
    slot.rtp_ts  = 0;
    slot.frames  = SLOT_FRAME_SAMPLES;
    slot.len     = SLOT_FRAME_LEN;
    memcpy(slot.payload, ciphertext, SLOT_FRAME_LEN);

    /* (a) decrypt + decode reproduces the reference S16 exactly -- proves the whole
     * unwrap -> decrypt -> decode chain end to end. */
    int16_t pcm[16384];
    int n = session_decode_slot(s, &slot, pcm, (int)(sizeof pcm / sizeof pcm[0]));
    assert(n == (int)SLOT_REF_N);
    for (unsigned i = 0; i < SLOT_REF_N; i++) assert(pcm[i] == SLOT_REF_S16[i]);

    /* (b) partial-accept sink: fake commits at most 100 samples/call -- write_all
     * must loop until every sample is committed, calling the sink ceil(n/100) times,
     * and the sink must have received exactly the decoded PCM in order. */
    sink_rec_max_accept  = 100;
    sink_rec_write_calls = 0;
    sink_rec_n           = 0;
    uint32_t gen = session_current_gen(s);
    int committed = session_sink_write_all(s, pcm, n, gen);
    assert(committed == n);
    int expected_calls = (n + 99) / 100;
    assert(sink_rec_write_calls == expected_calls);
    assert((int)sink_rec_n == n);
    assert(memcmp(sink_rec_buf, pcm, (size_t)n * sizeof(int16_t)) == 0);

    /* (c) abort_gen mismatched with the current gen from the start -> returns early
     * having committed nothing, without ever touching the sink. */
    sink_rec_max_accept  = (size_t)-1;
    sink_rec_write_calls = 0;
    int aborted = session_sink_write_all(s, pcm, n, gen + 1);
    assert(aborted == 0);
    assert(sink_rec_write_calls == 0);

    session_destroy(s);

    printf("test_session_decode OK\n");
    return 0;
}
