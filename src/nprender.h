#ifndef SHAIRKINDLE_NPRENDER_H
#define SHAIRKINDLE_NPRENDER_H
/* prefix = dir for the state files (np-title.txt etc.) + where np-art.jpg lives (matches AIRPLAY_PREFIX).
   paint_on_start != 0 => render once immediately (startup splash) with no wake. */
int  nprender_start(const char *prefix, int paint_on_start);
void nprender_wake(void);                  /* signal the worker that npstate changed (cheap, RTSP-thread-safe) */
void nprender_stop(void);                  /* signal stop + join the worker (clean shutdown / for tests) */
#endif
