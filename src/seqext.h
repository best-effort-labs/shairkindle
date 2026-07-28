#ifndef SHAIRKINDLE_SEQEXT_H
#define SHAIRKINDLE_SEQEXT_H
#include <stdint.h>
typedef struct {
    uint64_t seq_ref;   /* extended value of the current 16-bit reference */
    uint64_t ts_ref;    /* extended value of the current 32-bit ts anchor */
    int      inited;
} seqext_t;
void     seqext_init(seqext_t *s, uint16_t ref_seq, uint32_t ref_ts);
int      seqext_seq (seqext_t *s, uint16_t wire, uint64_t *ext);
uint64_t seqext_ts  (seqext_t *s, uint32_t wire);
void     seqext_rebase(seqext_t *s, uint16_t new_seq, uint32_t new_ts);
#endif
