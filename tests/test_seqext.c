#include <assert.h>
#include <stdio.h>
#include "../src/seqext.h"

int main(void){
    seqext_t s;
    seqext_init(&s, 0xFFFE, 1000);
    uint64_t e;
    assert(seqext_seq(&s, 0xFFFE, &e)==0 && e==0xFFFE);      /* the reference    */
    assert(seqext_seq(&s, 0xFFFF, &e)==0 && e==0xFFFF);      /* +1               */
    assert(seqext_seq(&s, 0x0000, &e)==0 && e==0x10000);     /* WRAP: contiguous */
    assert(seqext_seq(&s, 0x0001, &e)==0 && e==0x10001);
    /* reordered earlier packet still extends correctly, reference not moved back */
    assert(seqext_seq(&s, 0xFFFF, &e)==0 && e==0xFFFF);
    assert(seqext_seq(&s, 0x0002, &e)==0 && e==0x10002);     /* forward again    */
    /* ambiguous: > 32767 away -> reject */
    assert(seqext_seq(&s, 0x8002, &e)==-1);

    /* timestamp extension wraps around its anchor */
    seqext_t t; seqext_init(&t, 0, 0xFFFFFFF0u);
    assert(seqext_ts(&t, 0xFFFFFFF0u)==0xFFFFFFF0ull);
    assert(seqext_ts(&t, 0x00000010u)==0x100000010ull);      /* +32 across wrap  */

    /* FLUSH rebase: new boundary becomes the reference */
    seqext_rebase(&s, 0x0100, 5000);
    assert(seqext_seq(&s, 0x0100, &e)==0 && e==0x0100);
    assert(seqext_seq(&s, 0x0101, &e)==0 && e==0x0101);
    printf("test_seqext OK\n");
    return 0;
}
