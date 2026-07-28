#include "raop_mdns.h"
#include <string.h>
#include <stdio.h>
int raop_build_txt(char *out, size_t cap, const char *const *extra, int n_extra) {
    static const char *base[] = {
        "txtvers=1","ch=2","cn=1","ek=1","et=0,1","sm=false",
        "sr=44100","ss=16","sv=false","tp=UDP","vn=3","pw=false",
        /* Identity/capability keys iOS's AirPlay picker requires to LIST the device
         * (a raw mDNS browse accepts anything; the picker does not). Masquerade as an
         * AirPort Express, like shairport-sync classic. am=model, vs=RAOP server ver,
         * da=digest-auth, md=metadata(text,artwork,progress), fv=firmware. */
        "am=AirPort4,107","vs=105.1","da=true","md=0,1,2","fv=76400.10"
    };
    size_t o = 0;
    for (unsigned i = 0; i < sizeof base/sizeof base[0]; i++) {
        size_t l = strlen(base[i]);
        if (o + l + 1 >= cap) return -1;
        if (o) out[o++] = '\n';
        memcpy(out + o, base[i], l); o += l;
    }
    for (int i = 0; i < n_extra; i++) {
        size_t l = strlen(extra[i]);
        if (o + l + 2 >= cap) return -1;
        out[o++] = '\n'; memcpy(out + o, extra[i], l); o += l;
    }
    out[o] = 0;
    return (int)o;
}
