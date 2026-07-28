#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/rtsp.h"
#include "../src/rtsp_msg.h"
#include "fixture_sdp.h"    /* reuse SDP_VALID template pieces */

/* Same known-good base64 constants as tests/test_sdp.c: 256-byte
 * rsaaeskey, 16-byte aesiv. */
static const char RSAKEY_B64[] =
  "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4"
  "OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2RlZmdoaWprbG1ub3Bx"
  "cnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6PkJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmq"
  "q6ytrq+wsbKztLW2t7i5uru8vb6/wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj"
  "5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+/w==";
static const char AESIV_B64[] = "ESIzRFVmd4iZqrvM3e7/AA==";

/* Build a full "ANNOUNCE ... \r\n\r\n<sdp>" request that sdp_parse will
 * ACCEPT (valid base64 rsaaeskey/aesiv + a stereo/44100/16 fmtp), with a
 * correct Content-Length. Returns the request length. */
int make_announce(char *out, size_t cap){
    char sdp[1024];
    int slen=snprintf(sdp,sizeof sdp,SDP_TEMPLATE,RSAKEY_B64,AESIV_B64);
    return snprintf(out,cap,
      "ANNOUNCE rtsp://x/1 RTSP/1.0\r\nCSeq: 2\r\n"
      "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
      slen, sdp);
}

static int feed(rtsp_session_t *s, const char *raw, char *resp, size_t cap, size_t *rl){
    rtsp_msg_t m; size_t used;
    assert(rtsp_msg_parse(raw, strlen(raw), &m, &used)==1);
    return rtsp_handle(s, &m, resp, cap, rl);
}

int main(void){
    uint8_t ip[4]={192,168,1,10}, mac[6]={0xDE,0xAD,0xBE,0xEF,0,1};
    rtsp_session_t s; rtsp_session_init(&s, ip, mac, 88200);
    rtsp_set_local_ports(&s, 6000, 6001, 6002);
    char resp[4096]; size_t rl;

    /* OPTIONS with Apple-Challenge (valid base64, decodes to 16 bytes)
     * -> 200 + Apple-Response + Public */
    assert(feed(&s,
      "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nApple-Challenge: AQIDBAUGBwgJCgsMDQ4PEA==\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(strstr(resp,"RTSP/1.0 200"));
    assert(strstr(resp,"Apple-Response:"));
    assert(strstr(resp,"CSeq: 1"));

    /* GET_PARAMETER before SETUP (still IDLE) -> 455, wrong-state */
    assert(feed(&s, "GET_PARAMETER rtsp://x/1 RTSP/1.0\r\nCSeq: 20\r\n\r\n",
      resp,sizeof resp,&rl)!=0);
    assert(strstr(resp,"455"));

    /* stray/duplicate TEARDOWN while IDLE (no session ever bound) -> still a
     * well-formed 200 with CSeq echoed, but must NOT emit ACT_TEARDOWN --
     * The daemon must not run real teardown side effects for a session that was
     * never ANNOUNCED/bound. */
    assert(feed(&s, "TEARDOWN rtsp://x/1 RTSP/1.0\r\nCSeq: 21\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(strstr(resp,"RTSP/1.0 200"));
    assert(strstr(resp,"CSeq: 21"));
    assert(s.state==RTSP_IDLE && s.action==ACT_NONE);

    /* ANNOUNCE (valid SDP body via the Task-1 fixture template) */
    char ann[2048];
    int an=make_announce(ann,sizeof ann);
    assert(an>0 && (size_t)an<sizeof ann);
    assert(feed(&s, ann, resp, sizeof resp, &rl)==0);
    assert(s.state==RTSP_ANNOUNCED);
    assert(s.have_sdp && s.sdp.channels==2);

    /* SETUP -> parses sender ports, replies server_port, action BIND */
    assert(feed(&s,
      "SETUP rtsp://x/1 RTSP/1.0\r\nCSeq: 3\r\n"
      "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=55001;timing_port=55002\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s.state==RTSP_SETUP);
    assert(s.sender_control_port==55001 && s.sender_timing_port==55002);
    assert(s.action==ACT_BIND_RTP);
    assert(strstr(resp,"server_port=6000"));

    /* RECORD -> anchor + start */
    assert(feed(&s,
      "RECORD rtsp://x/1 RTSP/1.0\r\nCSeq: 4\r\nRTP-Info: seq=1234;rtptime=567890\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s.state==RTSP_PLAYING);
    assert(s.rtp_seq==1234 && s.rtp_ts==567890);
    assert(s.action==ACT_START_PLAY);
    assert(strstr(resp,"Audio-Latency: 88200"));

    /* FLUSH -> boundary + action, self-loop (stays PLAYING) */
    assert(feed(&s,
      "FLUSH rtsp://x/1 RTSP/1.0\r\nCSeq: 5\r\nRTP-Info: seq=2000;rtptime=999999\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s.state==RTSP_PLAYING && s.action==ACT_FLUSH);
    assert(s.rtp_seq==2000);

    /* second sender's ANNOUNCE while active -> 453 (busy) */
    assert(feed(&s, ann, resp, sizeof resp, &rl)!=0);
    assert(strstr(resp,"453"));
    assert(strstr(resp,"CSeq: 2"));

    /* SET_PARAMETER volume -> ACT_SET_VOLUME, bounds-safe body scan */
    assert(feed(&s,
      "SET_PARAMETER rtsp://x/1 RTSP/1.0\r\nCSeq: 7\r\n"
      "Content-Type: text/parameters\r\nContent-Length: 11\r\n\r\nvolume: 0.0",
      resp,sizeof resp,&rl)==0);
    assert(s.action==ACT_SET_VOLUME);
    assert(s.pending_volume_pct==100);

    /* GET_PARAMETER echoes it back in the body */
    assert(feed(&s, "GET_PARAMETER rtsp://x/1 RTSP/1.0\r\nCSeq: 8\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(strstr(resp,"volume: 100"));

    /* NaN volume is rejected (no UB float->int conversion); prior value stands */
    assert(feed(&s,
      "SET_PARAMETER rtsp://x/1 RTSP/1.0\r\nCSeq: 9\r\n"
      "Content-Type: text/parameters\r\nContent-Length: 11\r\n\r\nvolume: nan",
      resp,sizeof resp,&rl)==0);
    assert(s.action==ACT_NONE);
    assert(s.pending_volume_pct==100);

    /* hardening: an undersized response buffer truncates, never overflows */
    char tiny[8]; size_t tl;
    feed(&s, "OPTIONS * RTSP/1.0\r\nCSeq: 10\r\n\r\n", tiny, sizeof tiny, &tl);
    assert(tl<=sizeof tiny);

    /* regression: rtptime >= 2^31 must survive as the full uint32_t value,
     * not clamp to INT_MAX -- kv_int's signed-long clamp corrupted the
     * RECORD/FLUSH anchor for half the RTP timestamp space (rtptime starts
     * at a random 32-bit value). Exercise both handlers on a fresh session. */
    rtsp_session_t s2; rtsp_session_init(&s2, ip, mac, 88200);
    rtsp_set_local_ports(&s2, 6000, 6001, 6002);
    char ann2[2048]; int an2=make_announce(ann2,sizeof ann2);
    assert(an2>0 && (size_t)an2<sizeof ann2);
    assert(feed(&s2, ann2, resp, sizeof resp, &rl)==0);
    assert(feed(&s2,
      "SETUP rtsp://x/1 RTSP/1.0\r\nCSeq: 30\r\n"
      "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=55001;timing_port=55002\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(feed(&s2,
      "RECORD rtsp://x/1 RTSP/1.0\r\nCSeq: 31\r\nRTP-Info: seq=1;rtptime=3000000000\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s2.rtp_ts==3000000000u);
    assert(feed(&s2,
      "FLUSH rtsp://x/1 RTSP/1.0\r\nCSeq: 32\r\nRTP-Info: seq=2;rtptime=4000000000\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s2.rtp_ts==4000000000u);

    /* TEARDOWN -> IDLE */
    assert(feed(&s, "TEARDOWN rtsp://x/1 RTSP/1.0\r\nCSeq: 6\r\n\r\n",
      resp,sizeof resp,&rl)==0);
    assert(s.state==RTSP_IDLE && s.action==ACT_TEARDOWN);
    printf("test_rtsp OK\n");
    return 0;
}
