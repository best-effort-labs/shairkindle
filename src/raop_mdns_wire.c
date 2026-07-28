/* Pure DNS-SD TXT wire conversion -- no sockets, no tinysvcmdns dependency.
   Kept in its own TU (separate from raop_mdns.c's live mdnsd_* path) so the
   host test links cleanly without pulling in undefined mdnsd_* symbols. */
#include "raop_mdns.h"
#include <string.h>

int raop_txt_to_wire(const char *txt, uint8_t *out, size_t cap, size_t *entry_off, int max_entries) {
    if (!txt) return -1;
    int n = 0;
    size_t o = 0;
    const char *p = txt;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 255) return -1;
        if (n >= max_entries) return -1;
        if (o + 1 + len > cap) return -1;
        entry_off[n] = o;
        out[o++] = (uint8_t)len;
        memcpy(out + o, p, len);
        o += len;
        n++;
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}
