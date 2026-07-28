/* tests/fixture_sdp.h — a valid iTunes-style ANNOUNCE SDP body template plus
 * the known-good 48-byte cookie for its fmtp.
 *
 * The fmtp numbers (frameLength=4096, compatibleVersion=0, bitDepth=16, pb=40,
 * mb=10, kb=14, channels=2, maxRun=0, maxFrameBytes=16388, avgBitRate=1411200,
 * sampleRate=44100) and SDP_EXPECT_COOKIE below are copied/derived verbatim
 * from tests/fixture_alac.c's COOKIE array -- that fixture's cookie already
 * round-tripped through the real ALAC decoder in foundations, so it is
 * ground truth for what sdp_parse's build_cookie must reproduce.
 */
#ifndef FIXTURE_SDP_H
#define FIXTURE_SDP_H
#include <stdint.h>

/* rsaaeskey/aesiv are %s placeholders filled by the caller with real base64
   (see tests/test_sdp.c). */
static const char SDP_TEMPLATE[] =
  "v=0\r\n"
  "o=iTunes 3413821438 0 IN IP4 192.168.1.5\r\n"
  "s=iTunes\r\n"
  "c=IN IP4 192.168.1.10\r\n"
  "t=0 0\r\n"
  "m=audio 0 RTP/AVP 96\r\n"
  "a=rtpmap:96 AppleLossless\r\n"
  "a=fmtp:96 4096 0 16 40 10 14 2 0 16388 1411200 44100\r\n"
  "a=rsaaeskey:%s\r\n"
  "a=aesiv:%s\r\n";

/* The 48-byte cookie the fmtp above must produce (from tests/fixture_alac.c). */
extern const uint8_t SDP_EXPECT_COOKIE[48];

#endif
