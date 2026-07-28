/* tests/test_rtp_wire.c (skeleton — implementer fills BE field writes via memcpy helpers) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/rtp.h"
static void be16(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v;}
static void be32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void be64(uint8_t*p,uint64_t v){for(int i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
int main(void){
    uint8_t pkt[64]; memset(pkt,0,sizeof pkt);
    pkt[0]=0x80; pkt[1]=0xE0; be16(pkt+2,1234); be32(pkt+4,567890); be32(pkt+8,0xAABBCCDD);
    /* 16-byte AES-CBC block payload (28 bytes total) */
    memset(pkt+12, 0xAA, 16);
    rtp_audio_t a;
    assert(rtp_parse_audio(pkt,28,&a)==0);
    assert(a.seq==1234 && a.rtptime==567890 && a.payload_len==16 && memcmp(a.payload,pkt+12,16)==0);
    /* reject packets too short for mandatory encryption block (8, 12, 20 all reject; 28 accepts) */
    assert(rtp_parse_audio(pkt,8,&a)==-1);                 /* < 12: minimum header */
    assert(rtp_parse_audio(pkt,20,&a)==-1);                /* 12..27: no room for block */

    uint8_t rr[64]; rr[0]=0x80; rr[1]=0xD6; be16(rr+2,0); memcpy(rr+4,pkt,28);   /* resend-resp wrapper */
    const uint8_t *inner; size_t ilen;
    assert(rtp_classify_control(rr,4+28,&inner,&ilen)==CTRL_RESEND_AUDIO && inner==rr+4 && ilen==28);

    /* too-short buffer rejection for rtp_classify_control */
    assert(rtp_classify_control(pkt,1,&inner,&ilen)==CTRL_UNKNOWN);

    uint8_t sy[20]; memset(sy,0,20); sy[0]=0x80; sy[1]=0xD4; be32(sy+4,111111); be64(sy+8, ntp_from_ns(2000000000ull,0));
    uint32_t rn; uint64_t nn;
    assert(rtp_parse_sync(sy,20,&rn,&nn)==0 && rn==111111);
    /* too-short buffer rejection for rtp_parse_sync */
    assert(rtp_parse_sync(sy,15,&rn,&nn)==-1);

    uint8_t treq[32]; uint64_t tx=ntp_from_ns(1234000000ull,0);
    assert(rtp_build_timing_request(tx,treq,sizeof treq)==32 && treq[1]==0xD2);  /* marker set */
    /* too-short buffer rejection for rtp_build_timing_request */
    uint8_t treq_small[31];
    assert(rtp_build_timing_request(tx,treq_small,31)==-1);

    /* a REQUEST must NOT parse as a reply (wrong type 0x52 vs 0x53) */
    uint64_t got, echo; assert(rtp_parse_timing_reply(treq,32,&got,&echo)==-1);
    /* too-short buffer rejection for rtp_parse_timing_reply (distinct from type check) */
    assert(rtp_parse_timing_reply(treq,31,&got,&echo)==-1);

    /* build a real reply: type 0xD3, sender tx NTP at [24:32], our echoed originate at [8:16] */
    uint8_t trep[32]; memset(trep,0,32); trep[0]=0x80; trep[1]=0xD3;
    uint64_t stx=ntp_from_ns(5678000000ull,0); be64(trep+24,stx);
    be64(trep+8,tx);   /* echo the request's transmit token (our request's tx NTP == [24:32] of treq) */
    assert(rtp_parse_timing_reply(trep,32,&got,&echo)==0 && got==stx && echo==tx);

    /* mismatched/stale originate echo: caller must be able to distinguish it from a match */
    uint8_t trep_bad[32]; memcpy(trep_bad,trep,32);
    be64(trep_bad+8, tx ^ 0xFFFFFFFFFFFFFFFFull);   /* a different (stale) token */
    uint64_t got2, echo2;
    assert(rtp_parse_timing_reply(trep_bad,32,&got2,&echo2)==0 && got2==stx && echo2!=tx);

    uint8_t res[8]; assert(rtp_build_resend(2000,3,res,sizeof res)==8 && res[1]==0xD5);  /* marker set */
    uint16_t f=(uint16_t)(res[4]<<8|res[5]), c=(uint16_t)(res[6]<<8|res[7]);
    assert(f==2000 && c==3);
    /* too-short buffer rejection for rtp_build_resend */
    uint8_t res_small[7];
    assert(rtp_build_resend(2000,3,res_small,7)==-1);

    /* audio-socket trust boundary: a control-type datagram must NOT parse as audio */
    uint8_t bad[32]; memset(bad,0,32); bad[0]=0x80; bad[1]=0xD4;   /* a sync packet */
    assert(rtp_parse_audio(bad,28,&a)==-1);
    printf("test_rtp_wire OK\n"); return 0;
}
