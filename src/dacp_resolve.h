/* src/dacp_resolve.h — resolve the sender's dynamic DACP TCP port via a direct
 * one-shot mDNS SRV query for the exact instance iTunes_Ctrl_<DACP-ID>._dacp._tcp.local.
 *
 * We already hold the DACP-ID (from RTSP), so we query SRV directly rather than
 * browsing _dacp._tcp PTR. The parser is a dedicated, hardened DNS reader — the
 * vendored tinysvcmdns parser is a responder that skips SRV and lacks compression
 * guards, and this is unauthenticated LAN input.
 *
 * Pure pieces (build/parse) are host-tested; the socket resolver is device-path. */
#ifndef SHAIRKINDLE_DACP_RESOLVE_H
#define SHAIRKINDLE_DACP_RESOLVE_H
#include <stddef.h>

/* Build a one-question mDNS SRV query for the exact instance `owner`
 * (e.g. "iTunes_Ctrl_56B2A4D4._dacp._tcp.local"), QU bit set, id = dnsid.
 * Returns packet length in buf[0..cap), or -1 on error. */
int dacp_srv_build_query(unsigned char *buf, size_t cap, const char *owner, unsigned short dnsid);

/* Parse an mDNS response packet[0..len). Scan ALL sections (answer/authority/
 * additional) for a type-SRV record whose owner name (case-insensitive,
 * compression-decoded) equals `owner`; on the first valid match set *port and
 * return 0. Returns -1 if none (malformed -> -1, never reads OOB). TTL 0 and
 * port 0 are treated as no-match. dnsid: if nonzero, the packet id must match. */
int dacp_srv_parse_response(const unsigned char *packet, size_t len,
                            const char *owner, unsigned short dnsid, unsigned *port);

/* One-shot resolve: multicast an SRV query for `owner` out `iface_ip_be`,
 * collect replies up to deadline_ms (mono_ms clock), return the port on the
 * first valid matching SRV, else 0. Never blocks past the deadline. (device) */
unsigned dacp_resolve_port(const char *owner, unsigned iface_ip_be, long deadline_ms);

#endif
