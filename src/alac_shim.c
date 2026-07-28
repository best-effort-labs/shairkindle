#include "alac_shim.h"

#include <stdlib.h>
#include <string.h>

#include "alac.h"

struct alac_dec {
    alac_file *alac;
    int channels;
    int frame_samples;
    int sample_bytes;   /* bytes per single-channel sample (sample_size/8) */
    int expected_tag;   /* top-3-bit ALAC element tag: CPE stereo=1, SCE mono=0 */
    size_t maxframe;    /* byte capacity of in_pad / decode tmp buffer */
    unsigned char *in_pad;  /* zero-padded copy of caller input (over-read guard) */
    unsigned char *tmp;     /* decoder output scratch */
};

alac_dec *alac_open(int frame_samples, int sample_size, int channels,
                    int sample_rate, const uint8_t *magic_cookie,
                    size_t cookie_len) {
    (void)sample_rate;
    if ((channels != 1 && channels != 2) || !magic_cookie || cookie_len < 48)
        return NULL;

    alac_dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;

    d->channels = channels;
    d->frame_samples = frame_samples;
    d->sample_bytes = sample_size / 8;
    d->expected_tag = (channels == 2) ? 1 : 0;

    /* Max decoded/encoded frame in bytes, with generous headroom. The decoder
     * over-reads short buffers, so in_pad must be >= any real compressed frame;
     * a decompressed frame can never exceed samples*channels*bytes, and a
     * compressed one is smaller, so this bound covers both. +1024 slack. */
    d->maxframe = (size_t)frame_samples * channels * (size_t)d->sample_bytes * 2 + 1024;

    d->in_pad = calloc(1, d->maxframe);
    d->tmp = calloc(1, d->maxframe);
    d->alac = alac_create(sample_size, channels);
    if (!d->in_pad || !d->tmp || !d->alac) {
        alac_close(d);
        return NULL;
    }

    /* alac_set_info parses the 48-byte cookie and allocates internal buffers. */
    alac_set_info(d->alac, (char *)(void *)magic_cookie);
    return d;
}

int alac_decode(alac_dec *d, const uint8_t *in, size_t in_len,
                int16_t *out, int out_cap_samples) {
    if (!d || !in || in_len == 0) return -1;

    /* Malformed guard: check the top-3-bit ALAC element tag matches the channel
     * layout we were opened for. 0xFF -> tag 7 -> reject. */
    int tag = (in[0] >> 5) & 0x7;
    if (tag != d->expected_tag) return -1;

    /* Over-read protection: alac_decode_frame reads past in_len on short/garbage
     * input, so hand it a zero-padded copy sized to the max frame, never the
     * caller's raw short pointer. Reject anything that couldn't be a real frame. */
    if (in_len > d->maxframe) return -1;
    memset(d->in_pad, 0, d->maxframe);
    memcpy(d->in_pad, in, in_len);

    int outsize = 0;
    alac_decode_frame(d->alac, d->in_pad, d->tmp, &outsize);  /* outsize in BYTES */
    if (outsize <= 0) return -1;

    int nsamples = outsize / 2;  /* S16 = 2 bytes/sample */
    if (nsamples > out_cap_samples) return -1;
    memcpy(out, d->tmp, (size_t)outsize);
    return nsamples;

    /* The guard catches element-tag mismatch and short-buffer over-read,
     * but does NOT fully validate an arbitrary ALAC bitstream — a well-formed tag
     * with a corrupt body can still mis-decode. Real RAOP frames are well-formed;
     * malformed-body coverage belongs in a dedicated decoder fuzz harness. */
}

void alac_close(alac_dec *d) {
    if (!d) return;
    if (d->alac) alac_free(d->alac);
    free(d->in_pad);
    free(d->tmp);
    free(d);
}
