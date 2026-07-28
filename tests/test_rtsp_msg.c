#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/rtsp_msg.h"

int main(void){
    const char *req =
      "ANNOUNCE rtsp://1.2.3.4/1 RTSP/1.0\r\n"
      "CSeq: 3\r\n"
      "Content-Type: application/sdp\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "v=0\r\n";
    rtsp_msg_t m; size_t used;
    int r = rtsp_msg_parse(req, strlen(req), &m, &used);
    assert(r==1);
    assert(used==strlen(req));
    assert(m.method_len==8 && memcmp(m.method,"ANNOUNCE",8)==0);
    assert(m.cseq==3);
    assert(m.body_len==5 && memcmp(m.body,"v=0\r\n",5)==0);
    /* case-insensitive lookup */
    const char *ct=rtsp_hdr(&m,"content-type"); assert(ct && memcmp(ct,"application/sdp",15)==0);

    /* partial: truncate before body complete -> 0 (need more) */
    assert(rtsp_msg_parse(req, strlen(req)-2, &m, &used)==0);

    /* Oversized-body framing: a SET_PARAMETER whose declared body is larger
     * than what the caller holds still returns 0 (need more) BUT now exposes
     * method + CSeq + header-block length (consumed) + declared body_len, so a
     * caller whose buffer can't hold the body can answer from the headers and
     * skip body_len bytes. (iOS pushes album art this way.) */
    const char *big_hdr =
      "SET_PARAMETER rtsp://x RTSP/1.0\r\n"
      "CSeq: 9\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: 40000\r\n"
      "\r\n";
    size_t hlen = strlen(big_hdr);
    char big[512]; memcpy(big, big_hdr, hlen); memset(big+hlen, 0x7f, 100);  /* 100 body bytes only */
    rtsp_msg_t mo; size_t uo = 12345;
    assert(rtsp_msg_parse(big, hlen+100, &mo, &uo)==0);   /* body incomplete */
    assert(mo.method_len==13 && memcmp(mo.method,"SET_PARAMETER",13)==0);
    assert(mo.cseq==9);
    assert(mo.body==NULL && mo.body_len==40000);          /* declared length exposed */
    assert(uo==hlen);                                     /* consumed = header-block length */

    /* Incomplete HEADERS (no blank line yet) must stay unframeable: return 0
     * without touching the caller's zeroed msg (body_len stays 0 -> a skip
     * caller treats it as un-answerable, not a phantom oversized body). */
    rtsp_msg_t mh; memset(&mh,0,sizeof mh); size_t uh=0;
    assert(rtsp_msg_parse("SET_PARAMETER rtsp://x RTSP/1.0\r\nCSeq: 9\r\n", 41, &mh, &uh)==0);
    assert(mh.body_len==0);

    /* two messages in one buffer: parse first, consume, parse second */
    char two[512];
    int n=snprintf(two,sizeof two,
      "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\nRECORD rtsp://x RTSP/1.0\r\nCSeq: 2\r\n\r\n");
    assert(rtsp_msg_parse(two,(size_t)n,&m,&used)==1 && m.cseq==1);
    assert(rtsp_msg_parse(two+used,(size_t)n-used,&m,&used)==1 && m.cseq==2);

    /* Content-Length security guard: negative/non-digit/overflowing values
     * must reject the whole message (-1), not drive body_len/consumed via
     * an unbounded atol-style parse. A revert to atol would happily accept
     * all three and pass this test only if these asserts were absent. */
    const char *bad_neg =
      "ANNOUNCE rtsp://1.2.3.4/1 RTSP/1.0\r\n"
      "CSeq: 3\r\n"
      "Content-Length: -5\r\n"
      "\r\n";
    assert(rtsp_msg_parse(bad_neg, strlen(bad_neg), &m, &used)==-1);

    const char *bad_nondigit =
      "ANNOUNCE rtsp://1.2.3.4/1 RTSP/1.0\r\n"
      "CSeq: 3\r\n"
      "Content-Length: abc\r\n"
      "\r\n";
    assert(rtsp_msg_parse(bad_nondigit, strlen(bad_nondigit), &m, &used)==-1);

    const char *bad_overflow =
      "ANNOUNCE rtsp://1.2.3.4/1 RTSP/1.0\r\n"
      "CSeq: 3\r\n"
      "Content-Length: 999999999999999999999\r\n"
      "\r\n";
    assert(rtsp_msg_parse(bad_overflow, strlen(bad_overflow), &m, &used)==-1);

    /* Header-table overflow guard: RTSP_MAX_HDRS (24) slots fill up with
     * CSeq + 23 filler headers, then a 25th header (Content-Length) arrives
     * with the table already full. The old behavior silently dropped it,
     * leaving the message parsed as bodyless (return 1) and desyncing the
     * stream on the following body bytes -- must now reject the whole
     * message instead of losing a framing-critical header. */
    char overflow[4096]; int oo=0;
    oo+=snprintf(overflow+oo,sizeof overflow-oo,"ANNOUNCE rtsp://x/1 RTSP/1.0\r\nCSeq: 3\r\n");
    for (int i=0;i<23;i++) oo+=snprintf(overflow+oo,(size_t)(sizeof overflow-oo),"X%02d: 1\r\n",i);
    oo+=snprintf(overflow+oo,(size_t)(sizeof overflow-oo),"Content-Length: 5\r\n\r\nv=0\r\n");
    assert(rtsp_msg_parse(overflow,(size_t)oo,&m,&used)==-1);

    /* a garbage CSeq is non-fatal: message still parses, cseq just stays -1 */
    const char *bad_cseq =
      "OPTIONS * RTSP/1.0\r\n"
      "CSeq: xyz\r\n"
      "\r\n";
    assert(rtsp_msg_parse(bad_cseq, strlen(bad_cseq), &m, &used)==1);
    assert(m.cseq==-1);

    printf("test_rtsp_msg OK\n");
    return 0;
}
