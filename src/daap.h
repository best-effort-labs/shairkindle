#ifndef SHAIRKINDLE_DAAP_H
#define SHAIRKINDLE_DAAP_H
#include <stddef.h>
#include <stdint.h>
typedef struct { char title[256]; char artist[256]; char album[256]; } daap_meta_t;
/* Best-effort container-aware parse of a DMAP/DAAP tagged blob. Fills any of
   minm/asar/asal found (bounded recursion, offset-checked, UTF-8 truncated).
   Absent fields stay "". *out is zeroed on entry. Always returns 0. */
int daap_parse(const uint8_t *buf, size_t len, daap_meta_t *out);
#endif
