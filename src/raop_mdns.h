#ifndef RAOP_MDNS_H
#define RAOP_MDNS_H
#include <stddef.h>
#include <stdint.h>
int raop_build_txt(char *out, size_t cap, const char *const *extra, int n_extra);
/* Pure: convert raop_build_txt's newline-joined "k=v\nk=v" list into DNS-SD
   length-prefixed TXT wire form (each entry: 1 length byte + bytes), packed
   contiguously into out. entry_off[i] receives entry i's start offset in out.
   Returns the entry count, or -1 if an entry exceeds 255 bytes, out/cap would
   overflow, or there are more than max_entries entries.
   Implemented in src/raop_mdns_wire.c, kept link-independent of the live
   tinysvcmdns path (raop_mdns_start/stop) so the pure host test doesn't need
   mdnsd_* symbols. NOTE: the live path does NOT consume this buffer -- see
   raop_mdns.c, which hands mdnsd_register_svc a plain char* array instead
   (it length-prefixes internally). */
int raop_txt_to_wire(const char *txt, uint8_t *out, size_t cap, size_t *entry_off, int max_entries);
/* live advertisement: mdnsd_start() + mdnsd_set_hostname(ip_be) + mdnsd_register_svc().
   ip_be is the A-record address -- pass the SAME resolved primary-interface IPv4 used
   for rtsp_session_init, so discovery and Apple-Challenge agree. */
int  raop_mdns_start(const char *name, unsigned port, const char *txt, uint32_t ip_be);
void raop_mdns_stop(void);
#endif
