/* src/dacp_state.h — DACP session credentials + resolved endpoint, shared between
 * the RTSP thread (captures Active-Remote/DACP-ID) and the control thread (issues
 * commands). Mutex + generation guarded: a session change bumps the generation so
 * an in-flight resolve/command can't act on the wrong (old) sender.
 *
 * This header exposes the pure validators plus the state API. Validators are
 * host-tested directly; the state struct is host-tested via the API. */
#ifndef SHAIRKINDLE_DACP_STATE_H
#define SHAIRKINDLE_DACP_STATE_H
#include <stddef.h>

/* Validate + UPPERCASE-normalize a DACP-ID: 1..16 hex chars from in[0..inlen).
 * Writes a NUL-terminated string to out[17]. Returns 0 ok, -1 reject (empty,
 * non-hex, or >16). */
int dacp_normalize_id(char out[17], const char *in, size_t inlen);

/* Sanitize an Active-Remote token: copy leading ASCII DIGITS from in[0..inlen)
 * into out[0..outcap) (NUL-terminated), stopping at the first non-digit (incl.
 * CR/LF). Returns 0 ok, -1 reject (empty result, or the digit run >= outcap). */
int dacp_sanitize_token(char *out, size_t outcap, const char *in, size_t inlen);

/* --- session state (mutex + generation) --- */
typedef struct {
    unsigned  generation;
    int       have_creds;              /* both id+token validated & published */
    char      dacp_id[17];
    char      active_remote[32];
    unsigned  resolved_port;           /* 0 = unresolved */
    unsigned  resolved_gen;            /* generation the port was resolved under */
    unsigned  peer_ip_be;              /* RTSP peer IPv4 (network order) = DACP Host */
} dacp_snapshot_t;

void dacp_state_init(void);
/* New session boundary: bump generation, wipe creds + resolved port. Call on
 * client accept, ANNOUNCE (BEFORE capturing that request's headers), teardown. */
void dacp_state_new_session(void);
/* Capture raw RTSP header values (pointers into the RTSP buffer, with lengths)
 * plus the RTSP peer IPv4 (network order). Validates + publishes under lock; a
 * NULL/invalid id/token leaves creds unpublished. Does NOT bump generation. */
void dacp_state_capture(const char *id, size_t idlen, const char *token, size_t tlen,
                        unsigned peer_ip_be);
/* Copy the current state under the lock. */
void dacp_state_snapshot(dacp_snapshot_t *out);
/* Publish a resolved port IFF `gen` still equals the live generation (else drop). */
void dacp_state_publish_port(unsigned gen, unsigned port);

#endif
