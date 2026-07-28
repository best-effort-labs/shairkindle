#include "nprender.h"
#include "npstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>

extern char **environ;

/* Off-loop render worker: the RTSP thread never blocks on this. It only calls
   np_publish_meta/np_publish_art/np_set_playing (see npstate.c) + nprender_wake()
   (cheap: lock, set a flag, cond_signal). All drawing -- state-file writes + fork/execv of
   the external renderer -- happens on THIS thread. A render failure only
   logs; it can never tear down the session or the RTSP framing.

   NOTE: pthread_cond_timedwait calls clock_gettime under the hood, which
   SIGSEGVs on this device's pre-vDSO musl (see raop_clock.c). Therefore
   the dirty-wait below uses a plain untimed pthread_cond_wait, and the
   burst-coalescing debounce uses nanosleep (no clock probe) instead of a
   timed wait. Do NOT introduce pthread_cond_timedwait or clock_gettime here. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static int g_dirty   = 0;
static int g_running = 0;
static int g_started = 0;      /* 1 only if the worker thread was actually spawned */
static pthread_t g_thread;
static char g_prefix[400];
static long g_debounce_ms = 300;   /* overridable via NPRENDER_DEBOUNCE_MS (test determinism) */

static void path_for(char *buf, size_t cap, const char *name){
    snprintf(buf, cap, "%s/%s", g_prefix, name);
}

/* Bound to a display line: strip to `cap-1` bytes and replace any control
   byte (<0x20 or 0x7f -- newlines, tabs, etc.) with a space so the renderer
   script always reads exactly one clean line per file. High bytes (UTF-8
   continuation/lead bytes, >=0x80) pass through untouched -- fbink handles
   UTF-8. This is the ONLY place network-controlled metadata text goes; it
   never reaches argv or a shell command line. */
static void sanitize_copy(char *dst, size_t cap, const char *src){
    size_t n = 0;
    for (; src[n] != 0 && n < cap - 1; n++){
        unsigned char c = (unsigned char)src[n];
        dst[n] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[n] = 0;
}

/* Bounded write; failure just logs and the caller skips this draw -- never
   fatal (invariant: display failure can't affect audio/RTSP). */
static int write_state_file(const char *path, const char *line){
    FILE *f = fopen(path, "w");
    if (!f){ fprintf(stderr, "nprender: fopen(%s) failed: %s\n", path, strerror(errno)); return -1; }
    int ok = (fputs(line, f) != EOF) && (fputc('\n', f) != EOF);
    fclose(f);
    if (!ok) fprintf(stderr, "nprender: write to %s failed\n", path);
    return ok ? 0 : -1;
}

/* Bounded reap of a forked renderer child: poll waitpid(WNOHANG) with a
   nanosleep between tries (clock-free -- no clock_gettime/timedwait on this
   device, see file header), capped at ~5s total. An e-ink full flash
   legitimately takes ~1-2s, so don't kill early. If still unreaped at the
   cap the render is treated as WEDGED: escalate SIGTERM (short bounded
   wait), then SIGKILL + a final WNOHANG reap attempt. This bounds every
   render, and therefore bounds nprender_stop()'s pthread_join -- a hung
   renderer can no longer block clean daemon shutdown. */
static void reap_bounded(pid_t pid){
    struct timespec tick = { 0, 20L * 1000L * 1000L }; /* 20ms */
    int status;

    for (int i = 0; i < 250; i++){        /* 250 * 20ms = ~5s cap */
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0 && errno == EINTR) continue;
        nanosleep(&tick, NULL);
    }
    fprintf(stderr, "nprender: render pid %d wedged past 5s cap, SIGTERM\n", (int)pid);
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++){         /* 50 * 20ms = ~1s */
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0 && errno == EINTR) continue;
        nanosleep(&tick, NULL);
    }
    fprintf(stderr, "nprender: render pid %d still alive after SIGTERM, SIGKILL\n", (int)pid);
    kill(pid, SIGKILL);
    waitpid(pid, &status, WNOHANG);       /* best-effort final reap */
}

static int do_render(const char *mode, const np_snapshot_t *snap){
    char titlebuf[129], artistbuf[129], albumbuf[129];
    sanitize_copy(titlebuf,  sizeof titlebuf,  snap->title);
    sanitize_copy(artistbuf, sizeof artistbuf, snap->artist);
    sanitize_copy(albumbuf,  sizeof albumbuf,  snap->album);

    char artpath[512], titlepath[512], artistpath[512], albumpath[512];
    path_for(artpath,    sizeof artpath,    "np-art.jpg");
    path_for(titlepath,  sizeof titlepath,  "np-title.txt");
    path_for(artistpath, sizeof artistpath, "np-artist.txt");
    path_for(albumpath,  sizeof albumpath,  "np-album.txt");

    if (write_state_file(titlepath,  titlebuf)  != 0) return -1;
    if (write_state_file(artistpath, artistbuf) != 0) return -1;
    if (write_state_file(albumpath,  albumbuf)  != 0) return -1;

    /* Only hand the renderer an art path when THIS generation actually has
       art (snap->have_art). np-art.jpg on disk can be leftover from a
       previous track/session (a new track with no art, or art that hasn't
       arrived yet within the debounce window, leaves the old file in place)
       -- passing it unconditionally would redraw stale art. "" makes the
       script's `[ -f "$art" ]` check fail, so no art is drawn. */
    const char *ap = snap->have_art ? artpath : "";

    /* NO SHELL INJECTION: argv is a FIXED list of paths + the mode token.
       Metadata text is never interpolated into a command line -- it only
       ever reaches the renderer via the files just written above. */
    const char *renderer = getenv("AIRPLAY_NOWPLAYING_BIN");
    if (!renderer || !renderer[0]) renderer = "/var/local/shairkindle/airplay-nowplaying";
    char *argv[] = { (char *)renderer, (char *)mode, (char *)ap, titlepath, artistpath, albumpath, NULL };

    /* posix_spawn, NOT fork()+execv: on musl posix_spawn is clone(CLONE_VM|
       CLONE_VFORK), so the child shares our address space and the kernel never
       marks our pages copy-on-write. A raw fork() COWs the whole process --
       including the ~1MB session struct (its inline jitter buffer) -- and the
       concurrent rx_thread's next write (s->audio_pkts++) then needs a COW page;
       on the memory-tight K3 (2.6.26, ~7MB free) that allocation fails and the
       kernel SIGSEGVs the writer. The aplay sink already spawns via posix_spawn
       for the same reason; the render worker must too. exec-failure still exits
       127 inside the child, which reap_bounded observes as a normal exit. */
    pid_t pid;
    int sp = posix_spawn(&pid, renderer, NULL, NULL, argv, environ);
    if (sp != 0){ fprintf(stderr, "nprender: posix_spawn failed: %s\n", strerror(sp)); return -1; }
    reap_bounded(pid);
    return 0;
}

static void *worker_main(void *arg){
    (void)arg;
    unsigned long last_drawn_gen = 0;
    unsigned long seen_gen = (unsigned long)-1;   /* sentinel: never dedup the first pass */
    unsigned long seen_art_serial = 0;
    int seen_playing = -1;
    int last_layout = -1;     /* -1 none, 0 splash, 1 card */
    int last_have_art = -1;   /* whether the card's art region last showed real cover */

    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_dirty && g_running) pthread_cond_wait(&g_cond, &g_lock);
        if (!g_running){ pthread_mutex_unlock(&g_lock); break; }
        pthread_mutex_unlock(&g_lock);

        /* Debounce/coalesce: iOS pushes metadata + art as two separate bodies
           close together -- let a burst settle so it becomes ONE redraw
           (avoids a double e-ink flash). */
        long ms = g_debounce_ms;
        struct timespec debounce = { ms / 1000, (ms % 1000) * 1000L * 1000L };
        nanosleep(&debounce, NULL);

        /* Clear AFTER the debounce so wakes during this window are folded
           into the draw we're about to do; a wake during the draw itself
           re-sets dirty and is picked up next loop iteration. */
        pthread_mutex_lock(&g_lock);
        g_dirty = 0;
        pthread_mutex_unlock(&g_lock);

        np_snapshot_t snap;
        np_get(&snap);

        /* Skip a redundant render: if nothing that warrants a redraw changed
           since the last dispatch, don't spawn the renderer (no e-ink write).
           have_art is NOT in the key -- it's redundant: art arriving bumps
           art_serial, and a reset clearing have_art also bumps gen. */
        if (snap.gen == seen_gen && snap.art_serial == seen_art_serial && snap.playing == seen_playing)
            continue;
        /* Commit the dedup key ONLY on a dispatched render. A render that
           fails BEFORE drawing (state-file
           write or posix_spawn failure) leaves the key stale so the next
           identical wake retries -- otherwise a transient spawn failure on
           the memory-tight K3 would permanently suppress this track's draw
           (regression vs. the pre-dedup retry-every-wake behavior). A child
           that spawned but exited nonzero still counts as dispatched -- no
           infinite retry of a doomed cosmetic render. */
        /* Two visual layouts: splash (!playing) and card (playing). Flash the
           whole card band on any LAYOUT change, TRACK change (new gen), or
           ART-region change (glyph<->cover). Keying on gen alone is wrong:
           gen starts at 0, so a splash->card transition can occur at gen 0 and
           would light-draw the card over the still-visible splash. Track the
           last-rendered layout + have_art explicitly. */
        int dispatched = 0;
        if (!snap.playing) {
            /* splash mode region-flashes internally; nothing to compare */
            if (do_render("splash", &snap) == 0) { last_layout = 0; dispatched = 1; }
        } else {
            int have_art = snap.have_art ? 1 : 0;
            int flash = (last_layout != 1)               /* entering card */
                     || (snap.gen != last_drawn_gen)     /* new track */
                     || (have_art != last_have_art);     /* glyph<->cover */
            const char *mode = flash ? "flash" : "draw";
            if (do_render(mode, &snap) == 0) {
                last_layout = 1; last_drawn_gen = snap.gen; last_have_art = have_art;
                dispatched = 1;
            }
        }
        if (dispatched) {
            seen_gen = snap.gen; seen_art_serial = snap.art_serial; seen_playing = snap.playing;
        }
    }
    return NULL;
}

int nprender_start(const char *prefix, int paint_on_start){
    snprintf(g_prefix, sizeof g_prefix, "%s", prefix ? prefix : "");

    /* debounce override for deterministic tests (NPRENDER_DEBOUNCE_MS); default 300 */
    const char *dbg = getenv("NPRENDER_DEBOUNCE_MS");
    if (dbg && dbg[0]){
        int v = atoi(dbg);
        if (v < 0) v = 0;
        if (v > 10000) v = 10000;
        g_debounce_ms = v;
    }

    /* Gracefully disable (rather than spawn a worker that spews per-wake fopen/
       execv failures) when there's no usable place to render: no prefix, or the
       prefix dir doesn't exist. On the device /var/local/shairkindle exists; in bare
       test envs (e.g. rtsp_loopback with no AIRPLAY_PREFIX) it doesn't. */
    struct stat st;
    if (!g_prefix[0] || stat(g_prefix, &st) != 0 || !S_ISDIR(st.st_mode)){
        return 0;   /* not an error -- now-playing simply off */
    }
    g_dirty = paint_on_start ? 1 : 0;   /* 1 => first loop renders without a wake */
    g_running = 1;
    if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0){
        g_running = 0;
        return -1;
    }
    g_started = 1;
    return 0;
}

void nprender_wake(void){
    pthread_mutex_lock(&g_lock);
    g_dirty = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void nprender_stop(void){
    if (!g_started) return;   /* worker was never spawned (disabled) -- nothing to join */
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    g_dirty = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_started = 0;
}
