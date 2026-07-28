#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/bodycap.h"

static long fsize(const char*p){FILE*f=fopen(p,"rb"); if(!f)return -1; fseek(f,0,SEEK_END); long n=ftell(f); fclose(f); return n;}

int main(void){
    const char *out="/tmp/bodycap_test.bin"; unlink(out);
    bodycap_t b;
    bodycap_begin(&b, BODYCAP_FILE, 10, out, NULL, 0);
    assert(bodycap_feed(&b,(const uint8_t*)"hello",5)==5);
    assert(!bodycap_done(&b) && fsize(out)==-1);          /* no final file mid-stream */
    /* feed 8 more but only 5 remain: consumes 5, leaves 3 */
    assert(bodycap_feed(&b,(const uint8_t*)"world!!!",8)==5);
    assert(bodycap_done(&b));
    assert(bodycap_finish(&b)==0);
    assert(fsize(out)==10);
    FILE*f=fopen(out,"rb"); char buf[16]; size_t r=fread(buf,1,16,f); fclose(f);
    assert(r==10 && memcmp(buf,"helloworld",10)==0);

    /* MEM over-cap fails cleanly */
    uint8_t mem[4]; bodycap_t c; bodycap_begin(&c,BODYCAP_MEM,10,NULL,mem,sizeof mem);
    bodycap_feed(&c,(const uint8_t*)"abcdefghij",10);
    assert(c.failed==1);

    /* partial FILE finish leaves no final */
    const char *out2="/tmp/bodycap_test2.bin"; unlink(out2);
    bodycap_t d; bodycap_begin(&d,BODYCAP_FILE,10,out2,NULL,0);
    bodycap_feed(&d,(const uint8_t*)"abc",3);
    assert(!bodycap_done(&d)); assert(bodycap_finish(&d)==-1); assert(fsize(out2)==-1);

    unlink(out);
    printf("test_bodycap OK\n"); return 0;
}
