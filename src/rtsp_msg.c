/* src/rtsp_msg.c — RTSP request wire framing. Parses UNTRUSTED network
 * input: never trust buf beyond len, and never let a header value drive an
 * unbounded/overflowing size computation.
 */
#include "rtsp_msg.h"
#include <string.h>
#include <ctype.h>
#include <limits.h>

static int ci_eq(const char *a, size_t alen, const char *b){
    size_t bl=strlen(b);
    if (alen!=bl) return 0;
    for (size_t i=0;i<alen;i++) if (tolower((unsigned char)a[i])!=tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* Parse s[0..n) as a non-negative decimal long. Rejects empty input,
 * non-digit characters, and anything that would overflow `long` (attacker-
 * controlled Content-Length must never silently wrap). Returns 0 on
 * success, -1 otherwise. */
static int parse_nonneg_long(const char *s, size_t n, long *out){
    if (n==0) return -1;
    long v=0;
    for (size_t i=0;i<n;i++){
        char c=s[i];
        if (c<'0'||c>'9') return -1;
        int d=c-'0';
        if (v > (LONG_MAX-d)/10) return -1;  /* would overflow */
        v = v*10+d;
    }
    *out=v;
    return 0;
}

const char *rtsp_hdr(const rtsp_msg_t *m, const char *name){
    for (int i=0;i<m->n_hdr;i++) if (ci_eq(m->hdr[i].name,m->hdr[i].nlen,name)) return m->hdr[i].val;
    return NULL;
}

int rtsp_msg_parse(const char *buf, size_t len, rtsp_msg_t *m, size_t *consumed){
    /* find header/body separator "\r\n\r\n" */
    const char *hdrs_end=NULL;
    for (size_t i=0;i+3<len;i++)
        if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n'){ hdrs_end=buf+i+4; break; }
    if (!hdrs_end) return 0;                     /* need more */

    memset(m,0,sizeof *m); m->cseq=-1;
    /* request line */
    const char *p=buf;
    const char *sp1=memchr(p,' ',(size_t)(hdrs_end-p)); if(!sp1) return -1;
    m->method=p; m->method_len=(size_t)(sp1-p);
    const char *sp2=memchr(sp1+1,' ',(size_t)(hdrs_end-(sp1+1))); if(!sp2) return -1;
    m->uri=sp1+1; m->uri_len=(size_t)(sp2-(sp1+1));
    const char *line=memchr(sp2,'\n',(size_t)(hdrs_end-sp2)); if(!line) return -1;
    p=line+1;
    /* headers */
    long content_len=0;
    while (p < hdrs_end-2){
        const char *nl=memchr(p,'\n',(size_t)(hdrs_end-p)); if(!nl) break;
        const char *colon=memchr(p,':',(size_t)(nl-p));
        if (colon){
            /* Header table is full: a framing-critical header (e.g.
             * Content-Length) beyond slot RTSP_MAX_HDRS must never be
             * silently dropped -- that would parse the message as bodyless
             * and desync the stream on the following bytes. Reject instead. */
            if (m->n_hdr>=RTSP_MAX_HDRS) return -1;
            const char *v=colon+1; while (v<nl && (*v==' '||*v=='\t')) v++;
            const char *ve=nl; if (ve>v && ve[-1]=='\r') ve--;
            rtsp_hdr_t *h=&m->hdr[m->n_hdr++];
            h->name=p; h->nlen=(size_t)(colon-p); h->val=v; h->vlen=(size_t)(ve-v);
            if (ci_eq(h->name,h->nlen,"CSeq")) {
                long cs;
                if (parse_nonneg_long(h->val,h->vlen,&cs)==0) m->cseq=cs;
            } else if (ci_eq(h->name,h->nlen,"Content-Length")) {
                /* Malformed/negative/overflowing Content-Length is a
                 * protocol violation, not "need more bytes" -- fail the
                 * message rather than let it drive body-size math. */
                if (parse_nonneg_long(h->val,h->vlen,&content_len)!=0) return -1;
            }
        }
        p=nl+1;
    }
    size_t hdr_block=(size_t)(hdrs_end-buf);
    size_t have_body=len-hdr_block;
    if ((long)have_body < content_len) {
        /* Body not fully buffered yet. Normally the caller (drain) just tops up
         * and retries. But a caller whose read buffer can't hold this body needs
         * the framing to skip it: iOS pushes album art / DAAP metadata as a
         * multi-KB SET_PARAMETER body we never use, and a body larger than the
         * read buffer would otherwise wedge framing forever. Expose method +
         * CSeq + headers + the declared body_len (body stays NULL) and set
         * *consumed to the header-block length so such a caller can answer from
         * the headers and discard body_len bytes. drain() ignores both on 0. */
        m->body = NULL;
        m->body_len = (size_t)content_len;
        *consumed = hdr_block;
        return 0;                                   /* body incomplete */
    }
    m->body = content_len>0 ? hdrs_end : NULL;
    m->body_len=(size_t)content_len;
    *consumed=(size_t)(hdrs_end-buf)+(size_t)content_len;
    return 1;
}
