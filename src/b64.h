#ifndef SHAIRKINDLE_B64_H
#define SHAIRKINDLE_B64_H
#include <stddef.h>
#include <stdint.h>

/* Decode a base64 string (with or without '=' padding). Returns 0 on
 * success with *out_len set to the number of decoded bytes, or -1 on
 * invalid character, bad input length, or out_cap overflow. */
int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                size_t *out_len);

#endif
