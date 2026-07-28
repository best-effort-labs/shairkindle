/* Host test for the off-loop render worker (src/nprender.c). Points the
   worker at tests/fake_nprender (built by the Makefile) via
   AIRPLAY_NOWPLAYING_BIN, so no real fbink/exec is touched. The fake logs
   its argv (pipe-joined) to FAKE_RENDER_LOG -- one line per invocation --
   which lets this test assert invocation counts (coalesce) and structural
   argv shape (fixed paths + mode token only, never metadata text) without
   any shell involved.

   NO-SHELL-INJECTION invariant, by construction: nprender_start/wake/stop
   never builds a command string. The metadata (title/artist/album) only
   ever reaches disk via write_state_file() -> np-title.txt etc; the child
   is spawned with fork()+execv() with a FIXED argv array of paths + a mode
   token. This test double-checks that by asserting the fake's logged argv
   never contains the metadata text (see the checks below) and always
   matches the exact expected fixed path strings. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/nprender.h"
#include "../src/npstate.h"

static char PREFIX[256];
static char LOGPATH[512], ARTPATH[512], TITLEPATH[512], ARTISTPATH[512], ALBUMPATH[512];

static long count_lines(const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long n = 0; int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

/* Bounded poll (cap ~3s) -- no fixed sleep, no flaky exact timing. */
static int wait_for_lines(const char *path, long want){
    for (int waited_ms = 0; waited_ms < 3000; waited_ms += 20){
        if (count_lines(path) >= want) return 1;
        usleep(20 * 1000);
    }
    return 0;
}

static void read_all(const char *path, char *buf, size_t cap){
    FILE *f = fopen(path, "r");
    if (!f){ buf[0] = 0; return; }
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
}

static char *chomp(char *s){
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = 0;
    return s;
}

/* Returns a pointer (into buf, destructively tokenized) to the LAST line. */
static char *last_line(char *buf){
    char *saved = NULL;
    for (char *tok = strtok(buf, "\n"); tok; tok = strtok(NULL, "\n")) saved = tok;
    return saved;
}

/* Split a pipe-joined argv log line into fields; returns field count. */
static int split_fields(char *line, char **fields, int max){
    int n = 0;
    for (char *tok = strtok(line, "|"); tok && n < max; tok = strtok(NULL, "|")) fields[n++] = tok;
    return n;
}

int main(void){
    mkdir("/tmp/nprender_test_dir", 0755); /* ignore EEXIST */
    snprintf(PREFIX, sizeof PREFIX, "/tmp/nprender_test_dir");
    snprintf(LOGPATH, sizeof LOGPATH, "%s/fake_render.log", PREFIX);
    snprintf(ARTPATH, sizeof ARTPATH, "%s/np-art.jpg", PREFIX);
    snprintf(TITLEPATH, sizeof TITLEPATH, "%s/np-title.txt", PREFIX);
    snprintf(ARTISTPATH, sizeof ARTISTPATH, "%s/np-artist.txt", PREFIX);
    snprintf(ALBUMPATH, sizeof ALBUMPATH, "%s/np-album.txt", PREFIX);
    unlink(LOGPATH); unlink(TITLEPATH); unlink(ARTISTPATH); unlink(ALBUMPATH); unlink(ARTPATH);

    setenv("FAKE_RENDER_LOG", LOGPATH, 1);
    setenv("AIRPLAY_NOWPLAYING_BIN", "tests/fake_nprender", 1);
    /* Deterministic coalesce window: 2s is comfortably wider than any
       scheduler stall between the two rapid wakes in the coalesce check
       below, so they reliably land in one debounce window (no flake). Must
       be set before nprender_start (read once at start). */
    setenv("NPRENDER_DEBOUNCE_MS", "2000", 1);

    /* --- disabled-start lifecycle, FIRST (before any real worker exists so
       file-static globals -- g_started/g_running/g_prefix -- stay clean for
       the real-worker phase below): prefix dir absent -> no worker spawned,
       wake/stop must complete promptly and the fake renderer must never run. */
    assert(nprender_start("/nonexistent/prefix/xyz", 0) == 0);
    nprender_wake();
    nprender_stop();
    assert(count_lines(LOGPATH) == 0);

    /* --- paint_on_start: a worker started with paint_on_start=1 renders once
       immediately, with NO external wake (dirty flag pre-set). Runs its own
       isolated worker instance (separate prefix/log dir) BEFORE the real-worker
       phase below starts, so nprender.c's file-static globals (g_started/
       g_running/g_prefix) stay clean for that phase (same reasoning as the
       disabled-start test just above). --- */
    {
        char pos_prefix[300], pos_log[512];
        snprintf(pos_prefix, sizeof pos_prefix, "/tmp/nprender_test_paint_on_start");
        mkdir(pos_prefix, 0755); /* ignore EEXIST */
        snprintf(pos_log, sizeof pos_log, "%s/fake_render.log", pos_prefix);
        unlink(pos_log);
        setenv("FAKE_RENDER_LOG", pos_log, 1);

        np_set_playing(0);   /* default; !playing -> mode "splash" */
        assert(nprender_start(pos_prefix, 1) == 0);
        /* no nprender_wake() here -- paint_on_start must drive the first render */
        assert(wait_for_lines(pos_log, 1));
        nprender_stop();
        assert(count_lines(pos_log) == 1);   /* exactly one render, no wake needed */

        char buf[4096]; read_all(pos_log, buf, sizeof buf);
        char *ll = last_line(buf);
        assert(ll != NULL);
        char linebuf[2048]; snprintf(linebuf, sizeof linebuf, "%s", ll);
        char *fields[8];
        int nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "splash") == 0);   /* !playing mode */

        setenv("FAKE_RENDER_LOG", LOGPATH, 1);   /* restore for the rest of this file */
    }

    /* --- layout transitions: splash (!playing) vs card (playing); flash keys
       on LAYOUT change / TRACK change (gen) / ART-region change (have_art),
       not gen alone. Runs here -- BEFORE the real-worker phase below touches
       npstate at all -- so gen is still genuinely 0 at step 2, reproducing the
       gen==0 bug: a splash->card transition at gen 0 must still flash even
       though snap.gen(0) == a fresh worker's last_drawn_gen(0). Own prefix/
       log/worker instance so its trackers (last_layout/last_have_art) start
       clean and it doesn't disturb the real-worker phase's dedup state. --- */
    {
        char lt_prefix[300], lt_log[512];
        snprintf(lt_prefix, sizeof lt_prefix, "/tmp/nprender_test_layout");
        mkdir(lt_prefix, 0755); /* ignore EEXIST */
        snprintf(lt_log, sizeof lt_log, "%s/fake_render.log", lt_prefix);
        unlink(lt_log);
        setenv("FAKE_RENDER_LOG", lt_log, 1);

        assert(nprender_start(lt_prefix, 0) == 0);

        char buf[4096], linebuf[2048], *fields[8];
        int nf;

        /* 1) not playing -> splash */
        np_set_playing(0);
        nprender_wake();
        assert(wait_for_lines(lt_log, 1));
        read_all(lt_log, buf, sizeof buf);
        snprintf(linebuf, sizeof linebuf, "%s", last_line(buf));
        nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "splash") == 0);

        /* 2) splash -> card, gen still 0 (no metadata published yet) -- MUST
           flash, not draw. The old gen-only check would compare gen(0) to a
           fresh worker's last_drawn_gen(0) and wrongly pick "draw", light-
           drawing the card over the still-visible splash. */
        np_set_playing(1);
        nprender_wake();
        assert(wait_for_lines(lt_log, 2));
        read_all(lt_log, buf, sizeof buf);
        snprintf(linebuf, sizeof linebuf, "%s", last_line(buf));
        nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "flash") == 0);

        /* 3) first metadata (gen bump) -- still card, new track -> flash */
        daap_meta_t lt_m; memset(&lt_m, 0, sizeof lt_m);
        strcpy(lt_m.title, "LayoutSongA");
        np_publish_meta(&lt_m);
        nprender_wake();
        assert(wait_for_lines(lt_log, 3));
        read_all(lt_log, buf, sizeof buf);
        snprintf(linebuf, sizeof linebuf, "%s", last_line(buf));
        nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "flash") == 0);

        /* 4) art arrives for same track (have_art 0->1) -> flash (glyph->cover) */
        np_publish_art();
        nprender_wake();
        assert(wait_for_lines(lt_log, 4));
        read_all(lt_log, buf, sizeof buf);
        snprintf(linebuf, sizeof linebuf, "%s", last_line(buf));
        nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "flash") == 0);

        /* 5) playback stops -> splash */
        np_set_playing(0);
        nprender_wake();
        assert(wait_for_lines(lt_log, 5));
        read_all(lt_log, buf, sizeof buf);
        snprintf(linebuf, sizeof linebuf, "%s", last_line(buf));
        nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "splash") == 0);

        nprender_stop();
        setenv("FAKE_RENDER_LOG", LOGPATH, 1);   /* restore for the rest of this file */
    }

    assert(nprender_start(PREFIX, 0) == 0);

    /* --- draw path: first track -> mode "flash" (new gen), fixed argv.
       Publish art too (have_art=1) since this block asserts the art path IS
       passed -- see the Fix-1 regression test further down for the
       have_art=0 (withhold stale art) case. */
    np_set_playing(1);
    daap_meta_t m; memset(&m, 0, sizeof m);
    strcpy(m.title, "Song"); strcpy(m.artist, "Artist"); strcpy(m.album, "Album");
    np_publish_meta(&m);
    np_publish_art();
    nprender_wake();
    assert(wait_for_lines(LOGPATH, 1));

    {
        char buf[4096]; read_all(LOGPATH, buf, sizeof buf);
        char *ll = last_line(buf);
        assert(ll != NULL);
        char linebuf[2048]; snprintf(linebuf, sizeof linebuf, "%s", ll);
        char *fields[8];
        int nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[0], "tests/fake_nprender") == 0);
        assert(strcmp(fields[1], "flash") == 0);
        assert(strcmp(fields[2], ARTPATH) == 0);
        assert(strcmp(fields[3], TITLEPATH) == 0);
        assert(strcmp(fields[4], ARTISTPATH) == 0);
        assert(strcmp(fields[5], ALBUMPATH) == 0);
        /* structural no-injection check: argv is ONLY fixed tokens/paths --
           the metadata text never shows up here. */
        for (int i = 0; i < nf; i++){
            assert(strstr(fields[i], "Song") == NULL);
            assert(strstr(fields[i], "Artist") == NULL);
            assert(strstr(fields[i], "Album") == NULL);
        }

        char tbuf[300]; read_all(TITLEPATH, tbuf, sizeof tbuf); chomp(tbuf);
        char abuf[300]; read_all(ARTISTPATH, abuf, sizeof abuf); chomp(abuf);
        char lbuf[300]; read_all(ALBUMPATH, lbuf, sizeof lbuf); chomp(lbuf);
        assert(strcmp(tbuf, "Song") == 0);
        assert(strcmp(abuf, "Artist") == 0);
        assert(strcmp(lbuf, "Album") == 0);
    }

    /* --- coalesce: a burst of wakes must fold into fewer invocations --- */
    long before = count_lines(LOGPATH);
    daap_meta_t m2; memset(&m2, 0, sizeof m2);
    strcpy(m2.title, "Song2"); strcpy(m2.artist, "Artist2"); strcpy(m2.album, "Album2");
    np_publish_meta(&m2);
    nprender_wake();
    np_publish_art();
    nprender_wake();
    assert(wait_for_lines(LOGPATH, before + 1));
    usleep(500 * 1000); /* settle window: the two wakes above are two back-to-back
                            synchronous calls with no scheduler gap between them, so
                            with the 2s debounce override (set above) they land in the
                            same window deterministically -- this just confirms no
                            second, later draw slips through. */
    long after = count_lines(LOGPATH);
    assert(after - before >= 1);
    assert(after - before < 2); /* 2 wakes coalesced into fewer than 2 draws */

    /* --- gate: not-playing draws "splash", not track content --- */
    before = count_lines(LOGPATH);
    np_set_playing(0);
    nprender_wake();
    assert(wait_for_lines(LOGPATH, before + 1));
    {
        char buf[4096]; read_all(LOGPATH, buf, sizeof buf);
        char *ll = last_line(buf);
        char linebuf[2048]; snprintf(linebuf, sizeof linebuf, "%s", ll);
        char *fields[8];
        int nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        assert(strcmp(fields[1], "splash") == 0);
    }

    /* --- sanitization: control bytes/newlines become spaces, one clean line --- */
    before = count_lines(LOGPATH);
    np_set_playing(1);
    daap_meta_t m3; memset(&m3, 0, sizeof m3);
    strcpy(m3.title, "A\nB\x01" "C"); strcpy(m3.artist, "Artist3"); strcpy(m3.album, "Album3");
    np_publish_meta(&m3);
    nprender_wake();
    assert(wait_for_lines(LOGPATH, before + 1));
    {
        char tbuf[300]; read_all(TITLEPATH, tbuf, sizeof tbuf);
        assert(strchr(tbuf, '\x01') == NULL);
        /* exactly one newline: the one write_state_file appended itself */
        int nls = 0; for (char *p = tbuf; *p; p++) if (*p == '\n') nls++;
        assert(nls == 1);
        chomp(tbuf);
        assert(strcmp(tbuf, "A B C") == 0);
    }

    /* --- Art persists across a metadata update. iOS pushes art BEFORE the track's
       metadata, so np_publish_meta must NOT clear have_art (an earlier model did,
       which dropped the art entirely). Asserted here: art seen this session is
       still handed to the renderer after a subsequent metadata change. (The
       tradeoff -- a genuinely art-less track briefly shows the prior track's art --
       is accepted; see src/npstate.c header.) --- */
    before = count_lines(LOGPATH);
    {
        FILE *f = fopen(ARTPATH, "w");
        assert(f != NULL);
        fputs("art-bytes-for-the-current-track", f);
        fclose(f);
    }
    np_set_playing(1);
    np_publish_art();               /* art arrives first (this device's ordering) */
    daap_meta_t m4; memset(&m4, 0, sizeof m4);
    strcpy(m4.title, "ArtTrack"); strcpy(m4.artist, "Artist4"); strcpy(m4.album, "Album4");
    np_publish_meta(&m4);           /* content change; must NOT clear have_art */
    nprender_wake();
    assert(wait_for_lines(LOGPATH, before + 1));
    {
        char buf[4096]; read_all(LOGPATH, buf, sizeof buf);
        char *ll = last_line(buf);
        assert(ll != NULL);
        char linebuf[2048]; snprintf(linebuf, sizeof linebuf, "%s", ll);
        char *fields[8];
        int nf = split_fields(linebuf, fields, 8);
        assert(nf == 6);
        /* fields[2] is the art arg ($2 to the renderer script); it must be the art
           PATH now, not the empty-token "<EMPTY>". */
        assert(strcmp(fields[2], "<EMPTY>") != 0);
    }

    /* --- dedup: a wake with NO state change (same gen, art_serial, playing)
       must draw NOTHING (no redundant e-ink write). --- */
    before = count_lines(LOGPATH);
    nprender_wake();                       /* nothing changed since the last draw */
    usleep((2000 + 500) * 1000);           /* full 2s debounce window + settle */
    assert(count_lines(LOGPATH) == before); /* deduped: no new invocation */

    /* --- art replacement on the SAME track (art_serial bumps, gen unchanged)
       STILL draws -- proves art-only updates aren't lost. --- */
    before = count_lines(LOGPATH);
    np_publish_art();                      /* bumps art_serial only */
    nprender_wake();
    assert(wait_for_lines(LOGPATH, before + 1));  /* art change -> a redraw */

    /* --- failed render must NOT poison the dedup key. Make np-title.txt a
       DIRECTORY so write_state_file's fopen("w") fails (EISDIR) -> do_render
       returns before spawning; the key must stay stale so an identical later
       wake (after we remove the blocker) still redraws. Regression guard for
       the transient-failure case on the memory-tight device. --- */
    before = count_lines(LOGPATH);
    unlink(TITLEPATH);                     /* currently exists as a file */
    assert(mkdir(TITLEPATH, 0755) == 0);   /* now a dir -> fopen("w") fails EISDIR */
    np_set_playing(1);
    daap_meta_t m5; memset(&m5, 0, sizeof m5);
    strcpy(m5.title, "FailTrack"); strcpy(m5.artist, "Artist5"); strcpy(m5.album, "Album5");
    np_publish_meta(&m5);                  /* new gen -> would draw, but write fails */
    nprender_wake();
    usleep((2000 + 500) * 1000);           /* worker attempts + fails the write */
    assert(count_lines(LOGPATH) == before);/* no spawn, nothing logged */
    assert(rmdir(TITLEPATH) == 0);         /* clear the blocker; fopen recreates the file */
    nprender_wake();                       /* identical state -> retries (key not poisoned) */
    assert(wait_for_lines(LOGPATH, before + 1));

    /* --- clean stop: must join without hanging (outer `timeout` in the
       Makefile / verify step catches a real hang as a nonzero exit) --- */
    nprender_stop();

    printf("test_nprender OK\n");
    return 0;
}
