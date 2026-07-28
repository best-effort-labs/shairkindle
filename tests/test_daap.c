#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "../src/daap.h"

/* one DMAP item: 4-char code + 4-byte big-endian length + payload */
static size_t item(uint8_t *p, const char *code, const void *val, uint32_t n){
    memcpy(p, code, 4);
    p[4]=(n>>24)&0xff; p[5]=(n>>16)&0xff; p[6]=(n>>8)&0xff; p[7]=n&0xff;
    if(n) memcpy(p+8, val, n);
    return 8+n;
}

int main(void){
    daap_meta_t m;

    /* flat: minm/asar/asal in sequence */
    uint8_t b[512]; size_t o=0;
    o+=item(b+o,"minm","Song Title",10);
    o+=item(b+o,"asar","The Artist",10);
    o+=item(b+o,"asal","The Album",9);
    daap_parse(b,o,&m);
    assert(strcmp(m.title,"Song Title")==0);
    assert(strcmp(m.artist,"The Artist")==0);
    assert(strcmp(m.album,"The Album")==0);

    /* nested: a container 'mlit' wrapping minm -> found via recursion */
    uint8_t inner[64]; size_t io=item(inner,"minm","Nested",6);
    uint8_t c[128]; size_t co=item(c,"mlit",inner,(uint32_t)io);
    memset(&m,0,sizeof m); daap_parse(c,co,&m);
    assert(strcmp(m.title,"Nested")==0);

    /* truncated length must not overread: claim 999 bytes, give 4 */
    uint8_t t[16]; memcpy(t,"minm",4); t[4]=0;t[5]=0;t[6]=3;t[7]=0xE7; memcpy(t+8,"abc",3);
    memset(&m,0,sizeof m); daap_parse(t,11,&m);   /* must not crash; title stays empty or "abc"-safe */

    /* over-long string truncated to buffer, still NUL-terminated */
    char big[600]; memset(big,'x',sizeof big);
    uint8_t bb[700]; size_t bo=item(bb,"minm",big,sizeof big);
    memset(&m,0,sizeof m); daap_parse(bb,bo,&m);
    assert(m.title[255]==0 && strlen(m.title)==255);

    /* missing fields stay empty */
    uint8_t d[32]; size_t so=item(d,"asal","OnlyAlbum",9);
    memset(&m,0,sizeof m); daap_parse(d,so,&m);
    assert(m.title[0]==0 && m.artist[0]==0 && strcmp(m.album,"OnlyAlbum")==0);

    /* Realistic multi-tag layout, mirroring the structure of a real iOS
     * x-dmap-tagged body observed on-device (mlit container wrapping an opaque
     * 8-byte 'mper' persistent-id + interleaved sibling text tags). Synthetic
     * content -- proves container recursion, sibling walking, AND that the
     * opaque 'mper' (leading non-printable bytes) is NOT mis-recursed as a
     * container. */
    uint8_t items[256]; size_t n=0;
    uint8_t mper[8] = {0,0,0,0,0x00,0x18,0x56,0xa6};   /* opaque persistent-id */
    n+=item(items+n,"mper",mper,8);
    n+=item(items+n,"asal","Test Album",10);
    n+=item(items+n,"asar","Test Artist",11);
    n+=item(items+n,"ascp","Test Composer",13);        /* sibling we don't extract */
    n+=item(items+n,"asgn","Genre",5);
    n+=item(items+n,"minm","Test Track",10);
    uint8_t caps1=1; n+=item(items+n,"caps",&caps1,1); /* 1-byte tail sibling */
    uint8_t ct[300]; size_t ctn=item(ct,"mlit",items,(uint32_t)n);
    memset(&m,0,sizeof m); daap_parse(ct,ctn,&m);
    assert(strcmp(m.title,"Test Track")==0);
    assert(strcmp(m.artist,"Test Artist")==0);
    assert(strcmp(m.album,"Test Album")==0);

    printf("test_daap OK\n");
    return 0;
}
