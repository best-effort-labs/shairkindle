#ifndef SHAIRKINDLE_BODYCAP_H
#define SHAIRKINDLE_BODYCAP_H
#include <stddef.h>
#include <stdint.h>

/* Streaming capture of an RTSP request body, decoupled from the read loop so a
   large SET_PARAMETER body (album art / DAAP metadata) never blocks framing.
   FILE:    streams to <final>.tmp, atomic-renames to <final> at finish iff done && !failed.
   MEM:     accumulates up to mem_cap (excess dropped, failed=1).
   DISCARD: drains bytes off the wire, keeps RTSP framed.
   feed() always decrements remaining by the consumed count (min(len,remaining)),
   even in DISCARD/failed, so the caller stays byte-aligned on the stream. */
typedef enum { BODYCAP_DISCARD, BODYCAP_FILE, BODYCAP_MEM } bodycap_mode_t;
typedef struct {
    bodycap_mode_t mode; long remaining; int failed;
    int fd; char tmp[256]; char final[256];              /* FILE */
    uint8_t *mem; size_t mem_cap, mem_len;               /* MEM  */
} bodycap_t;

void   bodycap_begin(bodycap_t*, bodycap_mode_t, long declared_len,
                     const char *final_path, uint8_t *mem, size_t mem_cap);
size_t bodycap_feed(bodycap_t*, const uint8_t *data, size_t len);  /* consumes min(len,remaining); returns consumed */
int    bodycap_done(const bodycap_t*);                             /* remaining==0 */
int    bodycap_finish(bodycap_t*);                                 /* 0 ok / -1 fail; FILE: fsync+rename or unlink tmp */
#endif
