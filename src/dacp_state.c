/* src/dacp_state.c — DACP credential validators (pure) + mutex/generation session
 * state. See src/dacp_state.h. The state API is added on top of the validators. */
#include "dacp_state.h"
#include <string.h>
#include <pthread.h>

static int hexval(char c, char *up) {
    if (c >= '0' && c <= '9') { *up = c; return 1; }
    if (c >= 'a' && c <= 'f') { *up = (char)(c - 'a' + 'A'); return 1; }
    if (c >= 'A' && c <= 'F') { *up = c; return 1; }
    return 0;
}

int dacp_normalize_id(char out[17], const char *in, size_t inlen) {
    if (!in || inlen == 0 || inlen > 16) return -1;
    for (size_t i = 0; i < inlen; i++) {
        char up;
        if (!hexval(in[i], &up)) return -1;
        out[i] = up;
    }
    out[inlen] = 0;
    return 0;
}

int dacp_sanitize_token(char *out, size_t outcap, const char *in, size_t inlen) {
    if (!out || outcap == 0 || !in) return -1;
    size_t j = 0;
    for (size_t i = 0; i < inlen && in[i] >= '0' && in[i] <= '9'; i++) {
        if (j + 1 >= outcap) return -1;     /* would overflow (leave room for NUL) */
        out[j++] = in[i];
    }
    if (j == 0) return -1;                  /* no leading digits */
    out[j] = 0;
    return 0;
}

/* --- session state --- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static dacp_snapshot_t g_st;

void dacp_state_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(&g_st, 0, sizeof g_st);
    pthread_mutex_unlock(&g_lock);
}

void dacp_state_new_session(void) {
    pthread_mutex_lock(&g_lock);
    unsigned gen = g_st.generation + 1;
    memset(&g_st, 0, sizeof g_st);
    g_st.generation = gen;
    pthread_mutex_unlock(&g_lock);
}

void dacp_state_capture(const char *id, size_t idlen, const char *token, size_t tlen,
                        unsigned peer_ip_be) {
    /* Validate outside the lock (no shared state touched). */
    char nid[17], ntok[32];
    int ok = (dacp_normalize_id(nid, id, idlen) == 0) &&
             (dacp_sanitize_token(ntok, sizeof ntok, token, tlen) == 0);
    pthread_mutex_lock(&g_lock);
    g_st.peer_ip_be = peer_ip_be;          /* peer is always current for the live client */
    if (ok) {
        /* If the identity CHANGED within the session (different controller on the
         * same RTSP client), the cached SRV port belongs to the old ID -- drop it
         * so the next command re-resolves. Generation is unchanged (this isn't a
         * new RTSP session); clearing resolved_port forces re-resolution. */
        int changed = !g_st.have_creds ||
                      strcmp(g_st.dacp_id, nid) != 0 ||
                      strcmp(g_st.active_remote, ntok) != 0;
        if (changed) {
            g_st.resolved_port = 0;
            g_st.resolved_gen = 0;
        }
        memcpy(g_st.dacp_id, nid, sizeof g_st.dacp_id);
        memcpy(g_st.active_remote, ntok, sizeof g_st.active_remote);
        g_st.have_creds = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void dacp_state_snapshot(dacp_snapshot_t *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

void dacp_state_publish_port(unsigned gen, unsigned port) {
    pthread_mutex_lock(&g_lock);
    if (gen == g_st.generation) {
        g_st.resolved_port = port;
        g_st.resolved_gen = gen;
    }
    pthread_mutex_unlock(&g_lock);
}
