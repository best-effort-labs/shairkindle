#ifndef RAOPD_SINK_H
#define RAOPD_SINK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int  sink_open(int rate, int vol_pct);
int  sink_write(const int16_t *pcm, int n_samples);   /* returns accepted samples */
long sink_driver_delay_frames(void);                   /* DMA/ALSA delay, -1 if n/a */
long sink_queued_frames(void);                          /* software ring, stereo frames */
/* aplay-backend diagnostics (record backend returns 0/0/1): */
long sink_pending_bytes(void);                          /* bytes queued in the pipe, -1 if n/a */
int  sink_respawns(void);                               /* aplay respawn count this session */
int  sink_child_alive(void);                            /* 1 if the sink child is up, else 0 */
void sink_set_volume(int pct);
void sink_flush(void);
void sink_close(void);
#ifdef __cplusplus
}
#endif
#endif
