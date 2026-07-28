#include <assert.h>
#include <string.h>
#include "raop_mdns.h"
static int has_line(const char *buf, const char *kv) {
    /* match kv as a full \n-delimited token */
    size_t klen = strlen(kv);
    const char *p = buf;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t seg = e ? (size_t)(e-p) : strlen(p);
        if (seg == klen && strncmp(p, kv, klen) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
int main(void) {
    char buf[512];
    int n = raop_build_txt(buf, sizeof buf, NULL, 0);
    assert(n > 0);
    assert(has_line(buf, "txtvers=1"));
    assert(has_line(buf, "cn=1"));
    assert(has_line(buf, "et=0,1"));
    assert(has_line(buf, "sr=44100"));
    assert(has_line(buf, "ss=16"));
    assert(has_line(buf, "ch=2"));
    assert(has_line(buf, "tp=UDP"));
    /* identity keys iOS's AirPlay picker needs to list the device */
    assert(has_line(buf, "am=AirPort4,107"));
    assert(has_line(buf, "vs=105.1"));
    assert(has_line(buf, "da=true"));
    assert(has_line(buf, "md=0,1,2"));
    /* too-small buffer -> -1, no overflow */
    assert(raop_build_txt(buf, 8, NULL, 0) == -1);
    return 0;
}
