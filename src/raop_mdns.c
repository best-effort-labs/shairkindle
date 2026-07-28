/* Live _raop._tcp mDNS advertisement over the vendored tinysvcmdns responder.
   The pure TXT-wire helper (raop_txt_to_wire, tested by
   tests/test_mdns_txt.c) lives in raop_mdns_wire.c; this file's socket path
   is exercised only by the device cross-build (make raopd-armv6) -- nothing
   here is host-tested (it needs a real multicast socket + a real listener).

   tinysvcmdns's actual API (mdnsd_register_svc) takes a NULL-terminated
   array of plain "k=v" C strings and length-prefixes each one itself
   (rr_add_txt -> create_label, which copies + prefixes). It does NOT want a
   pre-length-prefixed buffer like raop_txt_to_wire produces -- handing it
   that would double-encode. So split_txt() below turns the newline-joined
   txt into a plain char* array instead of routing through raop_txt_to_wire
   at all. */
#include "raop_mdns.h"
#include "mdnsd.h"
#include <stdlib.h>
#include <string.h>

static struct mdnsd *g_svr = NULL;
static struct mdns_service *g_svc = NULL;

#define MAX_TXT_ENTRIES 32

/* Splits a "k=v\nk=v" string into a NULL-terminated argv-style array, in a
   caller-owned strdup'd buffer (newlines replaced with '\0' in place).
   Caller frees the returned buffer once mdnsd_register_svc() returns (it
   copies each string via create_label, so nothing is retained). */
static char *split_txt(const char *txt, const char *argv[], int max) {
    char *buf = strdup(txt ? txt : "");
    if (!buf) return NULL;
    int n = 0;
    char *p = buf;
    if (*p) {
        argv[n++] = p;
        for (; *p && n < max - 1; p++) {
            if (*p == '\n') {
                *p = '\0';
                argv[n++] = p + 1;
            }
        }
    }
    argv[n] = NULL;
    return buf;
}

int raop_mdns_start(const char *name, unsigned port, const char *txt, uint32_t ip_be) {
    if (g_svr) return -1; /* already started */

    struct mdnsd *svr = mdnsd_start();
    if (!svr) return -1;

    static const char hostname[] = "shairkindle.local";
    mdnsd_set_hostname(svr, hostname, ip_be);

    const char *argv[MAX_TXT_ENTRIES];
    char *buf = split_txt(txt, argv, MAX_TXT_ENTRIES);
    if (!buf) {
        mdnsd_stop(svr);
        return -1;
    }

    struct mdns_service *svc = mdnsd_register_svc(svr, name, "_raop._tcp.local",
                                                    (uint16_t)port, hostname, argv);
    free(buf);
    if (!svc) {
        mdnsd_stop(svr);
        return -1;
    }

    g_svr = svr;
    g_svc = svc;
    return 0;
}

void raop_mdns_stop(void) {
    if (g_svc) {
        mdns_service_destroy(g_svc);
        g_svc = NULL;
    }
    if (g_svr) {
        mdnsd_stop(g_svr);
        g_svr = NULL;
    }
}
