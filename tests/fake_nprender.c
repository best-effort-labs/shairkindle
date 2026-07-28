/* Fake renderer for test_nprender.c: exec'd by nprender's worker in place of
   the installed /var/local/shairkindle/airplay-nowplaying. It does NOT
   read the metadata files -- it just appends its own argv (pipe-joined) as
   one log line, so the test can (a) count invocations (coalesce check) and
   (b) assert the argv it received was ONLY fixed paths + the mode token,
   never metadata text (no-shell-injection structural check). The test reads
   the state files (np-title.txt etc.) directly to verify sanitized content. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv){
    const char *log = getenv("FAKE_RENDER_LOG");
    if (!log) return 1;
    FILE *f = fopen(log, "a");
    if (!f) return 1;
    /* An empty argv[i] (e.g. the art arg when have_art=0) would vanish under
       a pipe-joined "||" and confuse strtok-based field splitting in the
       test -- log it as the distinct, non-empty token "<EMPTY>" instead so
       the test can assert an arg was specifically empty. */
    for (int i = 0; i < argc; i++)
        fprintf(f, "%s%s", i ? "|" : "", argv[i][0] ? argv[i] : "<EMPTY>");
    fprintf(f, "\n");
    fclose(f);
    return 0;
}
