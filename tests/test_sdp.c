#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/sdp.h"
#include "fixture_sdp.h"

/* base64 of bytes 0x00..0xff (256 bytes) -- decodes to exactly 256 bytes. */
static const char RSAKEY_B64[] =
  "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4"
  "OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2RlZmdoaWprbG1ub3Bx"
  "cnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6PkJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmq"
  "q6ytrq+wsbKztLW2t7i5uru8vb6/wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj"
  "5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+/w==";

/* base64 of 16 arbitrary bytes -- decodes to exactly 16 bytes. */
static const char AESIV_B64[] = "ESIzRFVmd4iZqrvM3e7/AA==";

/* Copied verbatim from tests/fixture_alac.c's COOKIE (48 bytes). */
const uint8_t SDP_EXPECT_COOKIE[48] = {
  0x00,0x00,0x00,0x0c,0x66,0x72,0x6d,0x61,0x61,0x6c,0x61,0x63,0x00,0x00,0x00,0x24,
  0x61,0x6c,0x61,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x10,0x28,0x0a,
  0x0e,0x02,0x00,0x00,0x00,0x00,0x40,0x04,0x00,0x15,0x88,0x80,0x00,0x00,0xac,0x44,
};

int main(void) {
    /* --- b64 round-trip sanity --- */
    uint8_t buf[256];
    size_t n;
    assert(b64_decode(AESIV_B64, strlen(AESIV_B64), buf, sizeof buf, &n) == 0);
    assert(n == 16);
    uint8_t aesiv_expect[16];
    memcpy(aesiv_expect, buf, 16);
    assert(b64_decode("!!!!", 4, buf, sizeof buf, &n) == -1);   /* invalid char */
    assert(b64_decode("AA=A", 4, buf, sizeof buf, &n) == -1);   /* data after padding */

    /* --- build the SDP body from the fixture template + keys --- */
    char body[2048];
    int len = snprintf(body, sizeof body, SDP_TEMPLATE, RSAKEY_B64, AESIV_B64);
    assert(len > 0 && (size_t)len < sizeof body);

    raop_sdp_t s;
    assert(sdp_parse(body, (size_t)len, &s) == SDP_OK);
    assert(s.payload_type == 96);
    assert(s.frame_length == 4096);
    assert(s.bit_depth == 16);
    assert(s.channels == 2);
    assert(s.sample_rate == 44100);
    assert(s.rsaaeskey_len == 256);
    assert(memcmp(s.aesiv, aesiv_expect, 16) == 0);          /* aesiv decoded correctly */
    assert(s.cookie_len == 48);
    assert(memcmp(s.cookie, SDP_EXPECT_COOKIE, 48) == 0);    /* cookie built right */

    /* --- rejections --- */
    char bad[2048];
    raop_sdp_t t;
    /* mono -> SDP_ERR_FORMAT */
    snprintf(bad, sizeof bad, "%s", body);
    char *ch = strstr(bad, "14 2 0 16388");
    assert(ch);
    ch[3] = '1';  /* channels 2->1 */
    assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_FORMAT);
    /* missing aesiv -> SDP_ERR_PARSE */
    snprintf(bad, sizeof bad, "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
      "a=fmtp:96 4096 0 16 40 10 14 2 0 16388 1411200 44100\r\na=rsaaeskey:%s\r\n",
      RSAKEY_B64);
    assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_PARSE);
    /* fmtp trailing garbage after the 12 numbers -> SDP_ERR_FMTP */
    snprintf(bad, sizeof bad, "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
      "a=fmtp:96 4096 0 16 40 10 14 2 0 16388 1411200 44100 999\r\n"
      "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", RSAKEY_B64, AESIV_B64);
    assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_FMTP);
    /* frameLength=0 (out of range) -> SDP_ERR_FMTP */
    snprintf(bad, sizeof bad, "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
      "a=fmtp:96 0 0 16 40 10 14 2 0 16388 1411200 44100\r\n"
      "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", RSAKEY_B64, AESIV_B64);
    assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_FMTP);
    /* frameLength=0x20000000 (out of range) -> SDP_ERR_FMTP; guards the 32-bit
     * maxframe overflow in alac_shim.c (a crafted huge frameLength would wrap
     * maxframe to a tiny heap allocation while the decoder still writes the
     * full sample count into it). */
    snprintf(bad, sizeof bad, "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
      "a=fmtp:96 536870912 0 16 40 10 14 2 0 16388 1411200 44100\r\n"
      "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", RSAKEY_B64, AESIV_B64);
    assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_FMTP);
    /* rsaaeskey decoding to more than 256 bytes -> SDP_ERR_OVERFLOW */
    {
        char huge_rsa[512];
        memset(huge_rsa, 'A', sizeof huge_rsa - 1);
        huge_rsa[sizeof huge_rsa - 1] = '\0';
        snprintf(bad, sizeof bad, "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
          "a=fmtp:96 4096 0 16 40 10 14 2 0 16388 1411200 44100\r\n"
          "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", huge_rsa, AESIV_B64);
        assert(sdp_parse(bad, strlen(bad), &t) == SDP_ERR_OVERFLOW);
    }

    printf("test_sdp OK\n");
    return 0;
}
