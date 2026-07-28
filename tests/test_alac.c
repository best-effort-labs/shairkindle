#include <assert.h>
#include <stdint.h>
#include "alac_shim.h"

/* Fixture (tests/fixture_alac.c): one ALAC frame + its decoded S16 reference,
 * generated once via a lossless round-trip through the vendored alac.c.
 * Defines: FRAME[]/FRAME_LEN, COOKIE[]/COOKIE_LEN, REF_S16[]/REF_N,
 *          FRAME_SAMPLES, SAMPLE_SIZE, CHANNELS, SAMPLE_RATE. */
int main(void) {
    extern const uint8_t FRAME[]; extern const unsigned FRAME_LEN;
    extern const uint8_t COOKIE[]; extern const unsigned COOKIE_LEN;
    extern const int16_t REF_S16[]; extern const unsigned REF_N;
    extern const int FRAME_SAMPLES, SAMPLE_SIZE, CHANNELS, SAMPLE_RATE;

    alac_dec *d = alac_open(FRAME_SAMPLES, SAMPLE_SIZE, CHANNELS, SAMPLE_RATE,
                            COOKIE, COOKIE_LEN);
    assert(d != NULL);

    int16_t out[16384];
    int n = alac_decode(d, FRAME, FRAME_LEN, out, (int)(sizeof out / sizeof out[0]));
    assert(n == (int)REF_N);
    for (unsigned i = 0; i < REF_N; i++) assert(out[i] == REF_S16[i]);

    /* malformed frame must not crash and must return -1 */
    uint8_t bad[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    assert(alac_decode(d, bad, sizeof bad, out, 16384) == -1);

    /* Crafted stereo frames that pass the shim tag guard but set hassize=1 with an
     * oversized outputsamples, driving the heap overflow the clamp now prevents.
     * MSB-first bit layout matching the stereo branch of alac_decode_frame:
     *   tag=001 (3b) | skip(4b) | skip(12b) | hassize=1 | uncompressed=00(2b) |
     *   isnotcompressed=1 | outputsamples(32b), zero-padded.
     *
     * (a) outputsamples=0x00010000 (65536 > 4096 max) — a POSITIVE oversized value.
     * Without the clamp the uncompressed decode loop writes 65536 samples into the
     * 4096-sample outputsamples_buffer_a (heap overflow, ASan-confirmed); the clamp
     * rejects it -> -1. This is the case that genuinely fails if the clamp regresses.
     * Packs to: 0x20 0x00 0x12 0x00 0x02 0x00 0x00 ... */
    uint8_t malicious_pos[64] = {0x20, 0x00, 0x12, 0x00, 0x02, 0x00, 0x00};
    assert(alac_decode(d, malicious_pos, sizeof malicious_pos, out, 16384) == -1);

    /* (b) outputsamples=0xFFFFFFFF — the (uint32_t)-cast clamp still catches it.
     * Packs to: 0x20 0x00 0x13 0xFF 0xFF 0xFF 0xFE ... */
    uint8_t malicious_max[64] = {0x20, 0x00, 0x13, 0xFF, 0xFF, 0xFF, 0xFE};
    assert(alac_decode(d, malicious_max, sizeof malicious_max, out, 16384) == -1);

    alac_close(d);
    return 0;
}
