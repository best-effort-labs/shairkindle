#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "../src/npstate.h"

#define NTHREADS 4
#define NREADERS 2
#define ITERS 2000

/* Each writer publishes a CORRELATED tuple: title/artist/album all encode the
   same rotating token N ("tN"/"aN"/"lN"). A reader that ever observes a
   snapshot where the three suffixes disagree caught a torn/mixed read --
   the mutex's atomic-snapshot guarantee failed. Publishing identical
   constant strings (the old test) can never detect that. */
static void *writer(void *arg){
    (void)arg;
    daap_meta_t m; memset(&m,0,sizeof m);
    np_snapshot_t s;
    for (int i=0;i<ITERS;i++){
        int n = i % 97;
        snprintf(m.title,  sizeof m.title,  "t%d", n);
        snprintf(m.artist, sizeof m.artist, "a%d", n);
        snprintf(m.album,  sizeof m.album,  "l%d", n);
        np_publish_meta(&m);
        np_publish_art();
        np_set_playing(i & 1);
        np_get(&s);
    }
    return NULL;
}

static volatile int g_stop_readers = 0;

static void *reader(void *arg){
    (void)arg;
    np_snapshot_t s;
    while (!g_stop_readers){
        np_get(&s);
        if (s.title[0] && s.artist[0] && s.album[0]){
            int nt = atoi(s.title + 1);
            int na = atoi(s.artist + 1);
            int nl = atoi(s.album + 1);
            assert(nt == na && na == nl);
        }
    }
    return NULL;
}

int main(void){
    np_snapshot_t s;

    /* default state */
    np_get(&s);
    assert(s.gen == 0);
    assert(s.title[0]==0 && s.artist[0]==0 && s.album[0]==0);
    assert(s.have_art == 0);
    assert(s.playing == 0);

    /* publish meta -> gen 1, exact strings, have_art still 0 */
    daap_meta_t m; memset(&m,0,sizeof m);
    strcpy(m.title,"Song"); strcpy(m.artist,"Artist"); strcpy(m.album,"Album");
    np_publish_meta(&m);
    np_get(&s);
    assert(s.gen == 1);
    assert(strcmp(s.title,"Song")==0);
    assert(strcmp(s.artist,"Artist")==0);
    assert(strcmp(s.album,"Album")==0);
    assert(s.have_art == 0);

    /* publish art -> have_art 1. Re-sending the SAME metadata must NOT bump gen
       (dedup: no spurious "new track" flash) and must NOT clear have_art (iOS sends
       art before the track's meta; clearing dropped the art). */
    np_publish_art();
    np_get(&s);
    assert(s.have_art == 1);

    /* art_serial bumps on every publish_art (art replaced), independent of gen */
    unsigned long art0 = s.art_serial;
    np_publish_art();
    np_get(&s);
    assert(s.art_serial == art0 + 1);   /* bumped again even though metadata unchanged */
    assert(s.gen == 1);                  /* art publish never bumps gen */

    np_publish_meta(&m);            /* identical content */
    np_get(&s);
    assert(s.gen == 1);            /* deduped: unchanged */
    assert(s.have_art == 1);       /* preserved: meta never clears art */

    /* a genuine content change bumps gen; have_art still preserved */
    daap_meta_t m2; memset(&m2,0,sizeof m2);
    strcpy(m2.title,"Song2"); strcpy(m2.artist,"Artist"); strcpy(m2.album,"Album");
    np_publish_meta(&m2);
    np_get(&s);
    assert(s.gen == 2);
    assert(strcmp(s.title,"Song2")==0);
    assert(s.have_art == 1);

    /* playing flag */
    np_set_playing(1);
    np_get(&s);
    assert(s.playing == 1);
    np_set_playing(0);
    np_get(&s);
    assert(s.playing == 0);

    /* gen bumps once per CONTENT change, not per call */
    unsigned long prev = s.gen;
    np_publish_meta(&m2);          /* duplicate -> no bump */
    np_get(&s); assert(s.gen == prev);
    strcpy(m2.title,"Song3"); np_publish_meta(&m2);
    np_get(&s); assert(s.gen == prev+1); prev = s.gen;
    strcpy(m2.album,"Album2"); np_publish_meta(&m2);
    np_get(&s); assert(s.gen == prev+1);

    /* np_reset clears everything incl. art (cross-session), bumps gen */
    np_reset();
    np_get(&s);
    assert(s.have_art == 0);
    assert(s.title[0]==0);

    /* thread-safety smoke: writers publish correlated tuples, readers assert
       every snapshot is internally consistent (real atomic-snapshot check,
       not just NUL-termination) -- must complete cleanly. */
    pthread_t wth[NTHREADS], rth[NREADERS];
    g_stop_readers = 0;
    for (int i=0;i<NREADERS;i++) assert(pthread_create(&rth[i], NULL, reader, NULL)==0);
    for (int i=0;i<NTHREADS;i++) assert(pthread_create(&wth[i], NULL, writer, NULL)==0);
    for (int i=0;i<NTHREADS;i++) assert(pthread_join(wth[i], NULL)==0);
    g_stop_readers = 1;
    for (int i=0;i<NREADERS;i++) assert(pthread_join(rth[i], NULL)==0);

    /* final snapshot must be self-consistent: strings NUL-terminated within bounds */
    np_get(&s);
    assert(memchr(s.title, 0, sizeof s.title) != NULL);
    assert(memchr(s.artist, 0, sizeof s.artist) != NULL);
    assert(memchr(s.album, 0, sizeof s.album) != NULL);

    /* np_reset: wipes title/artist/album/have_art (cross-session teardown),
       does NOT touch playing, bumps gen. */
    daap_meta_t rm; memset(&rm,0,sizeof rm);
    strcpy(rm.title,"Stale"); strcpy(rm.artist,"Sender"); strcpy(rm.album,"A");
    np_publish_meta(&rm);
    np_publish_art();
    np_set_playing(1);
    np_get(&s);
    unsigned long gen_before_reset = s.gen;
    assert(s.have_art == 1);

    np_reset();
    np_get(&s);
    assert(s.title[0]==0 && s.artist[0]==0 && s.album[0]==0);
    assert(s.have_art == 0);
    assert(s.gen > gen_before_reset);      /* monotonic bump */
    assert(s.playing == 1);                /* np_reset leaves playing untouched */

    printf("test_npstate OK\n");
    return 0;
}
