/* src/rtsp.c — RTSP session state machine + response construction. Pure /
 * I-O free: only touches the already-parsed rtsp_msg_t and the caller's
 * response buffer, and emits an rtsp_action_t for the daemon to act on.
 *
 * Header values and the request body are NOT NUL-terminated (rtsp_msg_t
 * bounds them by vlen/body_len) -- every strstr/atoi/atof below runs on a
 * local, explicitly bounded, NUL-terminated copy. Never call those directly
 * on rtsp_hdr()'s return or on req->body.
 */
#include "rtsp.h"
#include "raop_crypto.h"     /* raop_apple_response */
#include "raop_volume.h"     /* raop_volume_db_to_pct */
#include "b64.h"             /* b64_decode (Apple-Challenge is base64) */
#include <string.h>
#include <strings.h>          /* strncasecmp */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>             /* isnan */

/* Bounded response-buffer append: `o` is always kept <= cap, so `cap-o`
 * never underflows and resp+o never runs past the caller's buffer, even if
 * a segment would have overflowed it (it is simply truncated). */
static size_t resp_vprintf(char *resp, size_t cap, size_t o, const char *fmt, ...){
    if (o>cap) o=cap;
    va_list ap; va_start(ap,fmt);
    int w=vsnprintf(resp+o,cap-o,fmt,ap);
    va_end(ap);
    if (w<0) return o;
    size_t add=(size_t)w, room=cap-o;
    return o+(add<room?add:room);
}
static size_t resp_append(char *resp, size_t cap, size_t o, const char *src, size_t n){
    if (o>cap) o=cap;
    size_t room=cap-o, take=n<room?n:room;
    if (take) memcpy(resp+o,src,take);
    return o+take;
}
static int mth(const rtsp_msg_t *m, const char *name){
    size_t n=strlen(name); return m->method_len==n && memcmp(m->method,name,n)==0;
}

/* Copy a header's value into a NUL-terminated local buffer, bounded by its
 * real length (m->hdr[i].vlen) -- rtsp_hdr() only returns a pointer, not a
 * length, so raw strstr/atoi on it would overread past the header value into
 * whatever follows it in the request buffer. Returns the value length, or -1
 * if the header is absent or doesn't fit in cap. */
static int rtsp_hdr_copy(const rtsp_msg_t *m, const char *name, char *out, size_t cap){
    size_t nlen=strlen(name);
    for (int i=0;i<m->n_hdr;i++){
        const rtsp_hdr_t *h=&m->hdr[i];
        if (h->nlen==nlen && strncasecmp(h->name,name,nlen)==0){
            if (h->vlen+1>cap) return -1;
            memcpy(out,h->val,h->vlen);
            out[h->vlen]=0;
            return (int)h->vlen;
        }
    }
    return -1;
}

/* find "key=NNN" in a NUL-terminated, already-bounded header value -> int,
 * def if absent. strtol (not atoi) so an attacker-sized digit run clamps to
 * LONG_MIN/MAX instead of hitting atoi's undefined overflow behavior. */
static int kv_int(const char *val, const char *key, int def){
    const char *p=strstr(val,key); if(!p) return def;
    long v=strtol(p+strlen(key),NULL,10);
    if (v>INT_MAX) v=INT_MAX; else if (v<INT_MIN) v=INT_MIN;
    return (int)v;
}

/* Same as kv_int but for values that span the full uint32_t range --
 * RTP-Info's rtptime= starts at a random 32-bit value, so routing it through
 * kv_int's signed-long clamp would lose the top half of the space (any
 * rtptime >= 2^31 comes back as INT_MAX). strtoul, clamp to UINT32_MAX. */
static uint32_t kv_u32(const char *val, const char *key, uint32_t def){
    const char *p=strstr(val,key); if(!p) return def;
    unsigned long v=strtoul(p+strlen(key),NULL,10);
    if (v>UINT32_MAX) v=UINT32_MAX;
    return (uint32_t)v;
}

void rtsp_session_init(rtsp_session_t *s, const uint8_t ip4[4], const uint8_t mac[6],
                       int audio_latency){
    memset(s,0,sizeof *s); s->state=RTSP_IDLE; s->pending_volume_pct=-1;
    s->audio_latency=audio_latency;
    memcpy(s->local_ip4,ip4,4); memcpy(s->local_mac,mac,6);
}
void rtsp_set_local_ports(rtsp_session_t *s,int sv,int ctl,int tim){
    s->server_port=sv; s->control_port=ctl; s->timing_port=tim;
}

int rtsp_handle(rtsp_session_t *s, const rtsp_msg_t *req,
                char *resp, size_t cap, size_t *resp_len){
    s->action=ACT_NONE;
    char body[512]; size_t bl=0; body[0]=0;
    char extra[1024]; size_t el=0; extra[0]=0;   /* method-specific headers */
    const char *status="200 OK";
    int rc=0;

    /* Apple-Challenge answerable on ANY request. The header value is base64
     * (bounded, non-NUL) -- copy it, decode it, and hand raop_apple_response
     * the raw challenge bytes (it does its own base64 of the response). */
    char chb64[256];
    int chn=rtsp_hdr_copy(req,"Apple-Challenge",chb64,sizeof chb64);
    if (chn>0){
        uint8_t raw[32]; size_t rawlen;
        if (b64_decode(chb64,(size_t)chn,raw,sizeof raw,&rawlen)==0 && rawlen>0){
            char b64[512];
            int n=raop_apple_response(raw,rawlen,s->local_ip4,s->local_mac,b64,sizeof b64);
            if (n>0) el+=(size_t)snprintf(extra+el,sizeof extra-el,"Apple-Response: %s\r\n",b64);
        }
    }

    if (mth(req,"OPTIONS")){
        el+=(size_t)snprintf(extra+el,sizeof extra-el,
          "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
          "OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
    }
    else if (mth(req,"ANNOUNCE")){
        if (s->state!=RTSP_IDLE){ status="453 Not Enough Bandwidth"; rc=-1; }
        else if (!req->body || sdp_parse(req->body,req->body_len,&s->sdp)!=SDP_OK){
            status="400 Bad Request"; rc=-1;
        } else { s->have_sdp=1; s->state=RTSP_ANNOUNCED; }
    }
    else if (mth(req,"SETUP")){
        char trbuf[256];
        int trn=rtsp_hdr_copy(req,"Transport",trbuf,sizeof trbuf);
        if (s->state!=RTSP_ANNOUNCED || trn<0){ status="455 Method Not Valid In This State"; rc=-1; }
        else {
            s->sender_control_port=kv_int(trbuf,"control_port=",0);
            s->sender_timing_port =kv_int(trbuf,"timing_port=",0);
            s->state=RTSP_SETUP; s->action=ACT_BIND_RTP;
            el+=(size_t)snprintf(extra+el,sizeof extra-el,
              "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;"
              "control_port=%d;timing_port=%d\r\nSession: 1\r\n",
              s->server_port,s->control_port,s->timing_port);
        }
    }
    else if (mth(req,"RECORD")){
        char ribuf[128];
        int rin=rtsp_hdr_copy(req,"RTP-Info",ribuf,sizeof ribuf);
        if (s->state!=RTSP_SETUP){ status="455 Method Not Valid In This State"; rc=-1; }
        else {
            if (rin>=0){ s->rtp_seq=(uint16_t)kv_int(ribuf,"seq=",0);
                         s->rtp_ts =kv_u32(ribuf,"rtptime=",0); }
            s->state=RTSP_PLAYING; s->action=ACT_START_PLAY;
            el+=(size_t)snprintf(extra+el,sizeof extra-el,"Audio-Latency: %d\r\n",s->audio_latency);
        }
    }
    else if (mth(req,"FLUSH")){
        char ribuf[128];
        int rin=rtsp_hdr_copy(req,"RTP-Info",ribuf,sizeof ribuf);
        if (s->state!=RTSP_PLAYING){ status="455 Method Not Valid In This State"; rc=-1; }
        else { if (rin>=0){ s->rtp_seq=(uint16_t)kv_int(ribuf,"seq=",0);
                            s->rtp_ts =kv_u32(ribuf,"rtptime=",0); }
               s->action=ACT_FLUSH; }
    }
    else if (mth(req,"SET_PARAMETER")){
        /* "volume: -14.5" body line -- bound the scan to body_len via a
         * local NUL-terminated copy instead of strstr/atof on req->body
         * directly (it is not NUL-terminated). */
        if (req->body && req->body_len>0){
            char b[256];
            size_t n=req->body_len < sizeof b-1 ? req->body_len : sizeof b-1;
            memcpy(b,req->body,n); b[n]=0;
            const char *v=strstr(b,"volume:");
            if (v){
                float db=(float)atof(v+7);
                /* reject non-finite (e.g. "volume: nan") -- converting NaN
                 * to int is undefined behavior. */
                if (!isnan(db)){
                    s->pending_volume_pct=raop_volume_db_to_pct(db);
                    s->action=ACT_SET_VOLUME;
                }
            }
        }
    }
    else if (mth(req,"GET_PARAMETER")){
        if (s->state!=RTSP_SETUP && s->state!=RTSP_PLAYING){ status="455 Method Not Valid In This State"; rc=-1; }
        else if (s->pending_volume_pct>=0)
            bl+=(size_t)snprintf(body+bl,sizeof body-bl,"volume: %d\r\n",s->pending_volume_pct);
    }
    else if (mth(req,"TEARDOWN")){
        /* Only emit the teardown side effect when there was an active
         * session to tear down -- a stray/duplicate TEARDOWN on an already
         * IDLE session (real AirPlay clients send these) must not trigger
         * the daemon's real teardown side effects. The field reset below is
         * harmless to repeat, so it stays unconditional. */
        if (s->state!=RTSP_IDLE) s->action=ACT_TEARDOWN;
        s->state=RTSP_IDLE; s->have_sdp=0; s->pending_volume_pct=-1;
        s->sender_control_port=0; s->sender_timing_port=0;
        s->rtp_seq=0; s->rtp_ts=0;
    }
    else { status="501 Not Implemented"; rc=-1; }

    /* assemble -- every step is bounds-clamped (see resp_vprintf/resp_append),
     * so a too-small caller buffer truncates instead of overrunning. */
    size_t o=0;
    o=resp_vprintf(resp,cap,o,"RTSP/1.0 %s\r\n",status);
    o=resp_vprintf(resp,cap,o,"CSeq: %ld\r\nServer: raopd/1.0\r\n",req->cseq);
    if (el) o=resp_append(resp,cap,o,extra,el);
    o=resp_vprintf(resp,cap,o,"Content-Length: %zu\r\n\r\n",bl);
    if (bl) o=resp_append(resp,cap,o,body,bl);
    *resp_len=o;
    return rc;
}
