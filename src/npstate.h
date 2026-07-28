#ifndef SHAIRKINDLE_NPSTATE_H
#define SHAIRKINDLE_NPSTATE_H
#include "daap.h"
typedef struct {
    unsigned long gen;          /* monotonic; bumps only on a genuine metadata content change */
    unsigned long art_serial;   /* monotonic; bumps on each np_publish_art (art replaced) */
    char title[256];
    char artist[256];
    char album[256];
    int  have_art;              /* 1 once art has been published; NOT tied to gen — survives a metadata change */
    int  playing;               /* 1 while an RTSP RECORD session is active */
} np_snapshot_t;

void np_publish_meta(const daap_meta_t *m);  /* bump gen on genuine change; copy title/artist/album; does NOT touch have_art */
void np_publish_art(void);                    /* mark art present + bump art_serial */
void np_set_playing(int on);                  /* set the playing flag */
void np_get(np_snapshot_t *out);              /* atomic snapshot copy */
void np_reset(void);                          /* clear title/artist/album/have_art + bump gen; leaves playing untouched */
#endif
