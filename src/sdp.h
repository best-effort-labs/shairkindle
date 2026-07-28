#ifndef SHAIRKINDLE_SDP_H
#define SHAIRKINDLE_SDP_H
#include <stddef.h>
#include <stdint.h>

enum {
    SDP_OK = 0,
    SDP_ERR_PARSE     = -1,  /* malformed line / missing required attribute   */
    SDP_ERR_FORMAT    = -2,  /* not stereo/44100/16, or payload-type mismatch */
    SDP_ERR_KEYLEN    = -3,  /* rsaaeskey/aesiv wrong decoded length          */
    SDP_ERR_FMTP      = -4,  /* fmtp field count/range bad                    */
    SDP_ERR_OVERFLOW  = -5   /* a field exceeds a fixed buffer                */
};

typedef struct {
    /* RSA-wrapped session key, exactly as decoded from a=rsaaeskey (base64).
       Still encrypted -- session creation feeds this to raop_oaep_unwrap_key. 2048-bit
       AirPort key => 256 bytes, but accept 128/256 (RSA modulus width). */
    uint8_t  rsaaeskey[256];
    size_t   rsaaeskey_len;
    uint8_t  aesiv[16];              /* a=aesiv, must decode to exactly 16     */
    int      payload_type;           /* dynamic PT from m=/rtpmap/fmtp (96)    */
    int      frame_length;           /* fmtp[0] e.g. 352 (frames per packet)   */
    int      bit_depth;              /* fmtp[2] must be 16                     */
    int      channels;               /* fmtp[6] must be 2                      */
    int      sample_rate;            /* fmtp[10] must be 44100                 */
    uint8_t  cookie[48];             /* built ALAC magic cookie for alac_open  */
    size_t   cookie_len;             /* == 48                                  */
} raop_sdp_t;

int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
               size_t *out_len);   /* declared here for test convenience; impl in b64.c */
int sdp_parse(const char *body, size_t body_len, raop_sdp_t *out);

#endif
