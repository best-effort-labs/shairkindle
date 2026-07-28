#include "seqext.h"

void seqext_init(seqext_t *s, uint16_t ref_seq, uint32_t ref_ts){
    s->seq_ref=ref_seq; s->ts_ref=ref_ts; s->inited=1;
}
void seqext_rebase(seqext_t *s, uint16_t new_seq, uint32_t new_ts){
    s->seq_ref=new_seq; s->ts_ref=new_ts; s->inited=1;
}
int seqext_seq(seqext_t *s, uint16_t wire, uint64_t *ext){
    int16_t d=(int16_t)(wire - (uint16_t)(s->seq_ref & 0xFFFF));
    /* > 32767 in either direction is ambiguous under 16-bit wrap. int16_t cast
       already folds to [-32768,32767]; guard the extreme negative too. */
    if (d==-32768) return -1;
    int64_t candidate=(int64_t)s->seq_ref + d;
    if (candidate<0) return -1;
    uint64_t v=(uint64_t)candidate;
    if (v > s->seq_ref) s->seq_ref=v;   /* advance reference only forward */
    *ext=v;
    return 0;
}
uint64_t seqext_ts(seqext_t *s, uint32_t wire){
    int32_t d=(int32_t)(wire - (uint32_t)(s->ts_ref & 0xFFFFFFFFu));
    int64_t candidate=(int64_t)s->ts_ref + d;
    uint64_t v=(uint64_t)candidate;
    if (v > s->ts_ref) s->ts_ref=v;
    return v;
}
