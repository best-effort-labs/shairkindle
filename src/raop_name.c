#include "raop_name.h"
#include <string.h>

static int is_ws(unsigned char c){ return c == ' ' || c == '\t'; }

size_t raop_name_sanitize(const char *in, char *out, size_t out_cap){
    static const char DEFAULT[] = "ShairKindle";
    /* Filter into a bounded scratch: drop controls/DEL/'.'/'@', collapse ws. */
    char tmp[RAOP_NAME_MAX_BYTES * 4 + 1];   /* generous; input is bounded by cap below */
    size_t n = 0;
    int pending_space = 0, seen = 0;
    if (in){
        for (const unsigned char *p = (const unsigned char *)in; *p && n < sizeof tmp - 1; p++){
            unsigned char c = *p;
            if (c < 0x20 || c == 0x7f || c == '.' || c == '@') continue;
            if (is_ws(c)){ if (seen) pending_space = 1; continue; }
            if (pending_space){ tmp[n++] = ' '; pending_space = 0; if (n >= sizeof tmp - 1) break; }
            tmp[n++] = (char)c; seen = 1;
        }
    }
    tmp[n] = 0;
    if (n == 0){ /* empty -> default */
        size_t d = strlen(DEFAULT);
        if (d > out_cap - 1) d = out_cap - 1;
        memcpy(out, DEFAULT, d); out[d] = 0; return d;
    }
    /* Truncate to 50 bytes on a UTF-8 boundary: never cut inside a multibyte
       sequence (bytes 0x80..0xBF are continuations). */
    size_t cap = RAOP_NAME_MAX_BYTES;
    if (cap > out_cap - 1) cap = out_cap - 1;
    if (n > cap){
        n = cap;
        while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) n--;  /* back off continuations */
    }
    memcpy(out, tmp, n); out[n] = 0;
    return n;
}
