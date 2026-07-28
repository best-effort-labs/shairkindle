#include "npstate.h"
#include <string.h>
#include <pthread.h>

/* Art/metadata ordering: the AirPlay sender pushes art and metadata as SEPARATE
   bodies with no correlation id, and on this device (iOS) art arrives BEFORE the
   track's metadata. So np_publish_meta must NOT clear have_art (an earlier version
   did, which dropped the art entirely): np-art.jpg on disk is always the most
   recent art, so we keep showing it. The only cost is a new track with NO art of
   its own briefly showing the prior track's art -- acceptable, and self-correcting
   once real art arrives. np_reset (new session) still clears everything.

   Metadata is also re-sent several times per track; np_publish_meta bumps `gen`
   (the "new track -> full flash" signal) ONLY on a genuine content change, so
   duplicate sends don't each trigger a redundant e-ink flash. */

static pthread_mutex_t np_lock = PTHREAD_MUTEX_INITIALIZER;
static np_snapshot_t np_state; /* zero-initialized: gen 0, empty strings, have_art 0, playing 0 */

static void take(char *dst, size_t cap, const char *src){
    size_t n = strlen(src);
    if (n > cap-1) n = cap-1;
    memcpy(dst, src, n); dst[n] = 0;
}

void np_publish_meta(const daap_meta_t *m){
    pthread_mutex_lock(&np_lock);
    if (strcmp(np_state.title,  m->title)  != 0 ||
        strcmp(np_state.artist, m->artist) != 0 ||
        strcmp(np_state.album,  m->album)  != 0) {   /* genuine change -> new gen */
        np_state.gen++;
        take(np_state.title,  sizeof np_state.title,  m->title);
        take(np_state.artist, sizeof np_state.artist, m->artist);
        take(np_state.album,  sizeof np_state.album,  m->album);
        /* NOT clearing have_art here is deliberate -- see the file header. */
    }
    pthread_mutex_unlock(&np_lock);
}

void np_publish_art(void){
    pthread_mutex_lock(&np_lock);
    np_state.have_art = 1;
    np_state.art_serial++;
    pthread_mutex_unlock(&np_lock);
}

void np_set_playing(int on){
    pthread_mutex_lock(&np_lock);
    np_state.playing = (on != 0);
    pthread_mutex_unlock(&np_lock);
}

void np_get(np_snapshot_t *out){
    pthread_mutex_lock(&np_lock);
    *out = np_state;
    pthread_mutex_unlock(&np_lock);
}

/* Cross-session teardown: wipe the previous sender's metadata so the next
   client's RECORD (playing=1, before it publishes anything) doesn't wake the
   render worker onto stale title/artist/album/art. Does NOT touch `playing`
   -- the caller (teardown_client) sets that separately. */
void np_reset(void){
    pthread_mutex_lock(&np_lock);
    np_state.gen++;
    np_state.title[0] = 0;
    np_state.artist[0] = 0;
    np_state.album[0] = 0;
    np_state.have_art = 0;
    pthread_mutex_unlock(&np_lock);
}
