#ifndef SHAIRKINDLE_ALAC_SHIM_H
#define SHAIRKINDLE_ALAC_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* Narrow shim over the vendored hairtunes ALAC decoder: configure from SDP
 * fmtp params + magic cookie, decode one ALAC frame -> interleaved S16. */

typedef struct alac_dec alac_dec;

/* Open a decoder. Returns NULL on unsupported params (channels not 1 or 2,
 * cookie_len < 48, or NULL cookie). magic_cookie is the 48-byte QuickTime
 * frma/alac ALACSpecificConfig. */
alac_dec *alac_open(int frame_samples, int sample_size, int channels,
                    int sample_rate, const uint8_t *magic_cookie,
                    size_t cookie_len);

/* Decode one frame -> interleaved S16 in `out` (capacity out_cap_samples
 * int16s). Returns the interleaved S16 sample count, or -1 on error. */
int alac_decode(alac_dec *d, const uint8_t *in, size_t in_len,
                int16_t *out, int out_cap_samples);

void alac_close(alac_dec *d);

#endif /* SHAIRKINDLE_ALAC_SHIM_H */
