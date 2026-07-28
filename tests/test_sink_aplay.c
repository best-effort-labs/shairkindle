#include "sink.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
int main(void){
    setenv("RAOPD_APLAY", "./tests/fake_consumer", 1);  /* argv[0] only; fast mode */
    setenv("RAOPD_FAKE_MODE", "fast", 1);
    assert(sink_open(44100, 50) == 0);
    assert(sink_child_alive() == 1);
    int16_t pcm[704]; memset(pcm, 0, sizeof pcm);
    long sent = 0;
    for (int i = 0; i < 200; i++) sent += sink_write(pcm, 704);   /* 200 periods */
    assert(sent == 200*704);          /* fast consumer accepts everything */
    sink_close();

    /* slow consumer: sink_write must return promptly (<=~250ms) even when the
       pipe backs up, accepting a partial (possibly 0) count -- never hang. */
    setenv("RAOPD_FAKE_MODE", "slow", 1);
    assert(sink_open(44100, 50) == 0);
    int16_t big[8192]; memset(big, 0, sizeof big);
    for (int i = 0; i < 50; i++) {
        int a;
        if (i == 5) {
            /* by now several 16KB writes have gone at slow-consumer drain of
               512B/50ms -- the pipe is saturated, so this call must hit the
               poll(POLLOUT) backpressure path. Prove the Task-2 cumulative
               WRITE_WAIT_MS(~200ms) bound actually holds: the call returns
               within a generous ceiling, not just "eventually" (that's what
               the outer `timeout 20` only proves). Upper-bound only --
               don't assert it dropped samples, a fast host may accept all. */
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            a = sink_write(big, 8192);
            gettimeofday(&t1, NULL);
            long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
            assert(elapsed_ms < 1000);
        } else {
            a = sink_write(big, 8192);
        }
        assert(a >= 0 && a <= 8192);
    }
    sink_close();   /* must return; a hang here fails the test by timeout */

    /* dying consumer: after it exits, sink_write respawns it and respawn count rises.
       die-after-4096 makes the child exit every 4096 bytes; keep writing until the sink
       has OBSERVED a death (waitpid) and respawned. A beat between writes lets the child
       get scheduled + become reapable -- undelayed writes alone race the child's
       exit->visibility and flake under container/CI load (see docs/tool-bugs/
       2026-07-23-shairkindle-test-sink-aplay-container-flaky.md). Bounded (~5s) so a real
       no-respawn bug still fails the assert rather than hanging. */
    setenv("RAOPD_FAKE_MODE", "die-after-4096", 1);
    assert(sink_open(44100, 50) == 0);
    memset(pcm, 0, sizeof pcm);
    for (int i = 0; i < 500 && sink_respawns() == 0; i++) { sink_write(pcm, 704); usleep(10000); }
    assert(sink_respawns() >= 1);
    sink_close();

    printf("test_sink_aplay OK\n"); return 0;
}
