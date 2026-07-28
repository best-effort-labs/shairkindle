#ifndef SHAIRKINDLE_RAOP_NAME_H
#define SHAIRKINDLE_RAOP_NAME_H
#include <stddef.h>

/* Max BYTES of the configurable name portion: the full RAOP instance label
   "<12-hex-MAC>@<name>" must be <= 63 bytes; the "MAC@" prefix is 13 bytes. */
#define RAOP_NAME_MAX_BYTES 50

/* Sanitize an AirPlay name for use as BOTH a DNS-SD instance-name tail and an
   fbink display string. Strips controls/DEL, drops '.' and '@', collapses and
   trims whitespace, truncates to RAOP_NAME_MAX_BYTES on a UTF-8 boundary, and
   falls back to "ShairKindle" if the result is empty. out_cap must be
   >= RAOP_NAME_MAX_BYTES + 1. Returns bytes written (excluding NUL). */
size_t raop_name_sanitize(const char *in, char *out, size_t out_cap);

#endif
