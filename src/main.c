/* src/main.c — raopd daemon entry point.
 *
 * Modes:
 *   --version        print version, exit.
 *   --smoke <port>   foreground, loopback RTSP loop only (no daemonize, no
 *                    mDNS). Used by tests/test_rtsp_loopback.sh on the host.
 *   (default)        daemonize + lock/pidfile + mDNS advert + RTSP loop.
 *
 * The RTSP loop is a single-active-client poll() over the listen fd plus one
 * client fd (the spec assumes one sender at a time). Parsed requests drive
 * rtsp_handle(); its rtsp_action_t is dispatched to the rtp/session/sink layer.
 *
 * SETUP port-ordering (HARD): rtsp_handle builds the SETUP Transport reply from
 * s.server_port/control_port/timing_port, so RTP MUST be bound and
 * rtsp_set_local_ports() called BEFORE rtsp_handle() runs for a SETUP request.
 * See handle_msg(): we open RTP on the first SETUP, then handle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>          /* strncasecmp */
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/times.h>          /* times() — clock_gettime is banned (zig/musl vDSO SIGSEGV) */
#include <netinet/in.h>
#include <netinet/tcp.h>        /* TCP_KEEPIDLE/KEEPINTVL/KEEPCNT */
#include <arpa/inet.h>
#if defined(__linux__)
#include <netpacket/packet.h>   /* struct sockaddr_ll, AF_PACKET */
#endif

#include "version.h"
#include "rtsp.h"
#include "rtsp_msg.h"
#include "rtp.h"
#include "session.h"
#include "sink.h"
#include "raop_mdns.h"
#include "raop_name.h"
#include "daemon.h"
#include "wakelock.h"
#include "daap.h"
#include "bodycap.h"
#include "npstate.h"
#include "nprender.h"
#include "dacp.h"
#include "dacp_state.h"
#include <pthread.h>

#define RTSP_PORT      5000
#define AUDIO_LATENCY  88200          /* 2 s @ 44.1k, reported in RECORD reply */
#define DEFAULT_VOL    50
#define RBUF_CAP       8192           /* one RTSP request (SDP fits easily)     */
#define HANDSHAKE_SECS 10             /* drop a client that connects but never RECORDs */
                                      /* within this many seconds (holds the single    */
                                      /* client slot forever otherwise). Env override:  */
                                      /* AIRPLAY_HANDSHAKE_SECS. Only enforced pre-      */
                                      /* session — an active stream may idle RTSP.      */
#define IFACE_WAIT_SECS 2              /* boot iface-ready poll — the  */
#define IFACE_WAIT_TRIES 30            /* daemon (KUAL toggle) can beat Wi-Fi up; */
                                       /* 30×2s = ~60s. Tune the budget on the K3.*/

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig) { (void)sig; g_stop = 1; }

/* --foreground: run the full daemon path but don't fork off / redirect stdio, and
 * narrate the RTSP exchange + audio-flow to stderr. Off = silent daemon (stderr
 * -> /dev/null), so LOGF is a no-op in normal operation. */
static int g_fg = 0;
#define LOGF(...) do { if (g_fg) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

/* ---- server state (single active client) ---- */
typedef struct {
    int             listen_fd;
    int             client_fd;        /* -1 when idle                          */
    clock_t         client_since;     /* times() at accept; handshake deadline */
    uint32_t        client_ip_be;     /* peer address, network byte order      */
    uint32_t        iface_ip_be;      /* RAOP-selected iface IP (DACP mDNS egress) */
    int             dacp_probed;      /* self-probe fired once this session     */
    char            rbuf[RBUF_CAP];
    size_t          rlen;
    bodycap_t       bcap;              /* streaming capture of an oversized/art/meta body */
    int             bcap_active;       /* a body capture is in flight                     */
    char            prefix[256];       /* AIRPLAY_PREFIX — where np-art.jpg lands          */
    uint8_t         metabuf[16384];    /* MEM sink for DAAP metadata bodies.     */
                                       /* 16K covers title/artist/album+tags; bump if real */
                                       /* captures overflow (failed=1 just drops excess).  */

    rtsp_session_t  rtsp;
    uint8_t         ip4[4];
    uint8_t         mac[6];

    rtp_sockets_t   socks;
    int             rtp_bound;         /* socks open, not yet owned by a session */
    session_t      *sess;              /* active player, else NULL              */
} server_t;

/* Full-write helper: RTSP responses are small but a partial write is still
 * possible on a slow/backpressured client. Returns 0 on success, -1 on error. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        return -1;
    }
    return 0;
}

/* Release everything tied to the current client/session and return to
 * listen-only. Ownership: if a session was started it OWNS the rtp sockets
 * (session_destroy closes them) — do NOT also rtp_close. If SETUP bound the
 * sockets but no session took them, we close them here. */
static void teardown_client(server_t *S) {
    LOGF("[raopd] teardown_client (sess=%s, rlen=%zu)\n", S->sess ? "ACTIVE" : "none", S->rlen);
    np_reset(); np_set_playing(0); nprender_wake();
    if (S->sess) { session_stop(S->sess); session_destroy(S->sess); S->sess = NULL; wakelock_release(); }
    else if (S->rtp_bound) { rtp_close(&S->socks); }
    S->rtp_bound = 0;
    if (S->client_fd >= 0) { close(S->client_fd); S->client_fd = -1; }
    S->client_ip_be = 0;
    S->rlen = 0;
    S->dacp_probed = 0;
    dacp_state_new_session();           /* clear DACP creds+endpoint for the next sender */
    if (S->bcap_active) { bodycap_finish(&S->bcap); S->bcap_active = 0; }  /* drop any tmp */
    /* fresh protocol state for the next sender */
    rtsp_session_init(&S->rtsp, S->ip4, S->mac, AUDIO_LATENCY);
}

/* Pick a capture sink from Content-Type: album art -> FILE, DAAP metadata ->
 * MEM, anything else (incl. oversized bodies we don't recognise) -> DISCARD
 * (drained off the wire to keep RTSP framed). */
static bodycap_mode_t body_mode(const rtsp_msg_t *m) {
    const char *ct = NULL; size_t ctlen = 0;
    for (int i = 0; i < m->n_hdr; i++)
        if (m->hdr[i].nlen == 12 && strncasecmp(m->hdr[i].name, "Content-Type", 12) == 0) {
            ct = m->hdr[i].val; ctlen = m->hdr[i].vlen; break;
        }
    if (ct && ctlen >= 10 && strncasecmp(ct, "image/jpeg", 10) == 0)               return BODYCAP_FILE;
    if (ct && ctlen >= 25 && strncasecmp(ct, "application/x-dmap-tagged", 25) == 0) return BODYCAP_MEM;
    return BODYCAP_DISCARD;
}

/* Arm a body capture for message m (declared_len bytes), feeding any bytes
 * already sitting in the read buffer. Sets bcap_active. */
static void begin_body_capture(server_t *S, const rtsp_msg_t *m, long declared,
                               const uint8_t *already, size_t already_len) {
    bodycap_mode_t mode = body_mode(m);
    char artpath[512]; artpath[0] = 0;
    if (mode == BODYCAP_FILE) snprintf(artpath, sizeof artpath, "%s/np-art.jpg", S->prefix);
    bodycap_begin(&S->bcap, mode, declared,
                  mode == BODYCAP_FILE ? artpath : NULL,
                  mode == BODYCAP_MEM ? S->metabuf : NULL,
                  mode == BODYCAP_MEM ? sizeof S->metabuf : 0);
    if (already_len) bodycap_feed(&S->bcap, already, already_len);
    S->bcap_active = 1;
}

/* Finalize a completed capture: rename the art file, or parse+log the metadata.
 * Never tears down the client — a capture failure is cosmetic. */
static void finish_body_capture(server_t *S) {
    int rc = bodycap_finish(&S->bcap);
    if (S->bcap.mode == BODYCAP_FILE) {
        LOGF("[raopd] art %s -> %s\n", rc == 0 ? "captured" : "capture FAILED", S->bcap.final);
        if (rc == 0) { np_publish_art(); nprender_wake(); }
    } else if (S->bcap.mode == BODYCAP_MEM && rc == 0) {
        /* Persist the raw DAAP blob (parallels np-art.jpg): feeds Part C and is
         * a real parser fixture. single write — the blob is a few KB
         * on a local fs; loop it if a truncated np-meta.bin ever shows up. */
        char mp[512]; snprintf(mp, sizeof mp, "%s/np-meta.bin", S->prefix);
        int fd = open(mp, O_CREAT|O_TRUNC|O_WRONLY, 0644);
        if (fd >= 0) { if (write(fd, S->bcap.mem, S->bcap.mem_len) < 0) {/*cosmetic*/} close(fd); }
        daap_meta_t meta; daap_parse(S->bcap.mem, S->bcap.mem_len, &meta);
        LOGF("[raopd] now-playing: title=\"%s\" artist=\"%s\" album=\"%s\" (%zu B raw)\n",
             meta.title, meta.artist, meta.album, S->bcap.mem_len);
        np_publish_meta(&meta); nprender_wake();
    }
    S->bcap_active = 0;
}

/* Dispatch one parsed request. Returns 0 to keep the client, -1 if the client
 * was torn down (caller must stop draining the buffer). */
/* DACP self-probe: fire one command on a detached thread so the ~2-3s resolve+HTTP
 * never stalls the RTSP loop. Diagnostic-only (env AIRPLAY_DACP_PROBE), the first
 * bench step that isolates "DACP works on this sender" from the relay/kindlet. */
struct dacp_probe_arg { dacp_cmd_t cmd; unsigned iface; };
static void *dacp_probe_thread(void *a) {
    struct dacp_probe_arg *p = (struct dacp_probe_arg *)a;
    dacp_send_command(p->cmd, p->iface);
    free(p);
    return NULL;
}

static int handle_msg(server_t *S, const rtsp_msg_t *m) {
    LOGF("[raopd] RTSP %.*s\n", (int)m->method_len, m->method);

    /* DACP (v2): ANNOUNCE = a new session, so invalidate any prior sender's
     * creds+endpoint FIRST, then capture THIS request's headers. Senders repeat
     * Active-Remote/DACP-ID on their requests; capture whenever both are present
     * (a missing/invalid field just leaves controls disabled). */
    if (m->method_len == 8 && memcmp(m->method, "ANNOUNCE", 8) == 0)
        dacp_state_new_session();
    {
        const char *ar = NULL, *id = NULL; size_t arl = 0, idl = 0;
        for (int i = 0; i < m->n_hdr; i++) {
            if (m->hdr[i].nlen == 13 && strncasecmp(m->hdr[i].name, "Active-Remote", 13) == 0) {
                ar = m->hdr[i].val; arl = m->hdr[i].vlen;
            } else if (m->hdr[i].nlen == 7 && strncasecmp(m->hdr[i].name, "DACP-ID", 7) == 0) {
                id = m->hdr[i].val; idl = m->hdr[i].vlen;
            }
        }
        if (ar && id) dacp_state_capture(id, idl, ar, arl, S->client_ip_be);
    }
    /* SETUP port-ordering: bind RTP + publish our ports BEFORE rtsp_handle so
     * the Transport reply carries a real server_port. */
    int is_setup = (m->method_len == 5 && memcmp(m->method, "SETUP", 5) == 0);
    if (is_setup && !S->rtp_bound && !S->sess) {
        if (rtp_open(&S->socks) == 0) {
            S->rtp_bound = 1;
            rtsp_set_local_ports(&S->rtsp, S->socks.server_port,
                                 S->socks.control_port, S->socks.timing_port);
        }
        /* rtp_open failure: server_port stays 0; the sender will reject SETUP. */
    }

    /* Normal path: a fully-buffered SET_PARAMETER art/metadata body. The
     * oversized path (frame_oversized) already armed capture (bcap_active),
     * so skip it here; volume SET_PARAMETER is DISCARD-classified and ignored. */
    if (!S->bcap_active && m->method_len == 13 && memcmp(m->method, "SET_PARAMETER", 13) == 0 &&
        m->body && m->body_len > 0 && body_mode(m) != BODYCAP_DISCARD) {
        begin_body_capture(S, m, (long)m->body_len, (const uint8_t *)m->body, m->body_len);
        finish_body_capture(S);        /* whole body fed in one shot -> done */
    }

    char resp[4096]; size_t rlen = 0;
    rtsp_handle(&S->rtsp, m, resp, sizeof resp, &rlen);
    if (write_all(S->client_fd, resp, rlen) != 0) { teardown_client(S); return -1; }

    switch (S->rtsp.action) {
    case ACT_START_PLAY: {
        if (!S->rtp_bound) break;
        int vol = S->rtsp.pending_volume_pct >= 0 ? S->rtsp.pending_volume_pct : DEFAULT_VOL;
        session_t *sess = session_create(&S->rtsp.sdp, vol);
        if (!sess) {                       /* bad key / decoder — drop RTP */
            LOGF("[raopd] RECORD: session_create FAILED (bad key / decoder)\n");
            if (S->rtp_bound) { rtp_close(&S->socks); S->rtp_bound = 0; }
            break;
        }
        /* session_start takes ownership of the rtp fds unconditionally. */
        int rc = session_start(sess, &S->socks, S->rtsp.rtp_seq, S->rtsp.rtp_ts,
                               S->client_ip_be, (uint16_t)S->rtsp.sender_control_port,
                               (uint16_t)S->rtsp.sender_timing_port);
        S->rtp_bound = 0;                  /* fds now belong to sess either way */
        if (rc != 0) { LOGF("[raopd] RECORD: session_start FAILED\n");
                       session_destroy(sess); break; }
        S->sess = sess;
        wakelock_acquire();                /* keep the K3 awake while playing */
        LOGF("[raopd] RECORD: play session started (vol=%d, ctl=%u tim=%u)"
             " AUDIO rate=%d frame_length=%d channels=%d\n",
             vol, (unsigned)S->rtsp.sender_control_port,
             (unsigned)S->rtsp.sender_timing_port,
             S->rtsp.sdp.sample_rate, S->rtsp.sdp.frame_length, S->rtsp.sdp.channels);
        np_set_playing(1); nprender_wake();

        /* DACP self-probe (diagnostic): once playing + creds captured, fire one
         * command against the sender and log the [DACP] trace. Env-gated. */
        {
            const char *pr = getenv("AIRPLAY_DACP_PROBE");
            if (pr && !S->dacp_probed) {
                dacp_cmd_t c; int ok = 1;
                if      (!strcmp(pr, "playpause")) c = DACP_PLAYPAUSE;
                else if (!strcmp(pr, "nextitem"))  c = DACP_NEXTITEM;
                else if (!strcmp(pr, "previtem"))  c = DACP_PREVITEM;
                else ok = 0;
                if (ok) {
                    S->dacp_probed = 1;
                    dacp_snapshot_t sn; dacp_state_snapshot(&sn);
                    LOGF("[DACP] probe: creds=%s id=%.4s gen=%u\n",
                         sn.have_creds ? "present" : "ABSENT",
                         sn.have_creds ? sn.dacp_id : "----", sn.generation);
                    struct dacp_probe_arg *pa = malloc(sizeof *pa);
                    if (pa) {
                        pa->cmd = c; pa->iface = S->iface_ip_be;
                        pthread_t t;
                        if (pthread_create(&t, NULL, dacp_probe_thread, pa) == 0) pthread_detach(t);
                        else free(pa);
                    }
                }
            }
        }
        break;
    }
    case ACT_FLUSH:
        if (S->sess) {
            session_stats_t st; session_get_stats(S->sess, &st);
            LOGF("[raopd] FLUSH at recv=%lu started=%d (seq=%u ts=%u)\n",
                 st.recv, st.started, (unsigned)S->rtsp.rtp_seq, S->rtsp.rtp_ts);
            session_flush(S->sess, S->rtsp.rtp_seq, S->rtsp.rtp_ts);
        } else LOGF("[raopd] FLUSH (no session)\n");
        break;
    case ACT_SET_VOLUME:
        if (S->sess) sink_set_volume(S->rtsp.pending_volume_pct);
        break;
    case ACT_TEARDOWN:
        teardown_client(S);
        return -1;
    case ACT_BIND_RTP:   /* binding already done above */
    case ACT_NONE:
    default:
        break;
    }
    return 0;
}

/* Drain complete RTSP messages out of the read buffer. */
static void drain(server_t *S) {
    size_t off = 0;
    while (off < S->rlen) {
        rtsp_msg_t m; size_t consumed;
        int r = rtsp_msg_parse(S->rbuf + off, S->rlen - off, &m, &consumed);
        if (r == 0) break;                 /* need more bytes */
        if (r < 0) { teardown_client(S); return; }   /* malformed -> drop conn */
        if (handle_msg(S, &m) != 0) return;          /* client torn down */
        off += consumed;
    }
    if (off > 0 && off < S->rlen) memmove(S->rbuf, S->rbuf + off, S->rlen - off);
    S->rlen -= off;
}

/* The read buffer is full and drain() parsed nothing: the head request's body
 * exceeds RBUF_CAP. iOS pushes album art / DAAP metadata as a multi-KB
 * SET_PARAMETER body we don't use, and blocks on our response. Answer it from
 * its (complete) headers and arm a skip of the remaining body. Returns 0 if
 * handled (client answered, skip armed, or client torn down by the answer), -1
 * if the buffer is full of an un-terminated header block (unframeable). */
static int frame_oversized(server_t *S) {
    rtsp_msg_t m; memset(&m, 0, sizeof m);
    size_t consumed = 0;
    int r = rtsp_msg_parse(S->rbuf, S->rlen, &m, &consumed);
    /* r==0 with body_len>0 = complete headers, body too big (the case we handle).
     * Anything else (incomplete headers -> m stays zeroed; malformed) is unframeable. */
    if (r != 0 || m.body_len == 0) return -1;
    size_t already = S->rlen - consumed;   /* body bytes already buffered after the headers */
    LOGF("[raopd] oversized %.*s Content-Length=%zu — answering from headers, capturing %ld body bytes\n",
         (int)m.method_len, m.method, m.body_len, (long)m.body_len - (long)already);
    begin_body_capture(S, &m, (long)m.body_len,
                       (const uint8_t *)(S->rbuf + consumed), already);  /* feed the partial */
    S->rlen = 0;                       /* buffer is entirely this msg's headers + partial body */
    handle_msg(S, &m);                 /* answers from headers; capture streams the rest */
    return 0;                          /* handled (handle_msg tears down on write failure) */
}

static void on_client_readable(server_t *S) {
    if (S->rlen == RBUF_CAP && !S->bcap_active) {             /* oversized req: answer + capture body */
        if (frame_oversized(S) != 0) teardown_client(S);
        return;
    }
    ssize_t n = recv(S->client_fd, S->rbuf + S->rlen, RBUF_CAP - S->rlen, 0);
    if (n == 0) {                                             /* disconnect */
        LOGF("[raopd] client closed TCP (recv=0)%s\n", S->sess ? " WHILE PLAYING" : "");
        teardown_client(S); return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        LOGF("[raopd] client recv error: %s\n", strerror(errno));
        teardown_client(S); return;
    }
    S->rlen += (size_t)n;

    if (S->bcap_active) {                                     /* streaming an oversized body */
        size_t rem = (size_t)S->bcap.remaining;
        size_t take = S->rlen < rem ? S->rlen : rem;
        bodycap_feed(&S->bcap, (const uint8_t *)S->rbuf, take);
        memmove(S->rbuf, S->rbuf + take, S->rlen - take);
        S->rlen -= take;
        if (!bodycap_done(&S->bcap)) return;                  /* more body to stream */
        finish_body_capture(S);                               /* rename art / parse meta */
    }
    drain(S);                                                 /* trailing pipelined bytes */
}

static void on_listen_readable(server_t *S) {
    struct sockaddr_in peer; socklen_t plen = sizeof peer;
    int fd = accept(S->listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;
    if (S->client_fd >= 0) { close(fd); return; }   /* single active client */
    /* Reap a peer that drops silently mid-stream (WiFi blip / iOS backgrounding without
     * RTSP TEARDOWN): kernel keepalive -> POLLHUP -> teardown. Best-effort; the old K3
     * kernel may ignore the per-socket knobs, which is fine. */
    { int on = 1;
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
#ifdef TCP_KEEPIDLE   /* Linux (K3 + build container); macOS spells it differently */
      int idle = 15, intvl = 5, cnt = 3;
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
    }
    S->client_fd = fd;
    S->client_since = times(NULL);
    S->client_ip_be = peer.sin_addr.s_addr;
    S->rlen = 0;
    dacp_state_new_session();           /* fresh DACP session for the new client */
}

/* Bind + listen the RTSP TCP port on `bind_ip_be`. Returns fd or -1. */
static int rtsp_listen(uint32_t bind_ip_be, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = bind_ip_be;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0) {
        close(fd); return -1;
    }
    return fd;
}

static void run_loop(server_t *S) {
    unsigned long last_pkts = 0;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    const char *hs = getenv("AIRPLAY_HANDSHAKE_SECS");
    clock_t handshake_ticks = (clock_t)((hs && *hs ? atol(hs) : HANDSHAKE_SECS) * hz);
    while (!g_stop) {
        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = S->listen_fd; pfd[nfd].events = POLLIN; nfd++;
        if (S->client_fd >= 0) { pfd[nfd].fd = S->client_fd; pfd[nfd].events = POLLIN; nfd++; }

        /* foreground: 1 s tick to narrate audio flow. daemon: block until an fd wakes us,
         * EXCEPT while a client is mid-handshake (connected, no session yet) -- then tick
         * so we can enforce the handshake deadline below. */
        int handshaking = (S->client_fd >= 0 && !S->sess);
        int r = poll(pfd, (nfds_t)nfd, (g_fg || handshaking) ? 1000 : -1);
        if (r < 0) { if (errno == EINTR) continue; break; }

        /* Handshake deadline: a client that connects but never reaches RECORD holds the
         * single client slot indefinitely (the daemon poll would otherwise never wake).
         * Only pre-session -- teardown_client on an established stream would drop live
         * audio, and RTSP legitimately idles for minutes once RTP is flowing. */
        if (handshaking && handshake_ticks > 0) {
            clock_t now = times(NULL);
            if (now != (clock_t)-1 && now - S->client_since > handshake_ticks) {
                LOGF("[raopd] handshake deadline: client idle without RECORD; dropping\n");
                teardown_client(S);
                continue;
            }
        }
        if (r == 0) {
            if (S->sess) {
                session_stats_t st; session_get_stats(S->sess, &st);
                LOGF("[raopd] stream: recv=%lu(+%lu/s) started=%d pops=%lu dec=%lu decfail=%lu sink_last=%ld"
                     " | sync=%lu treq=%lu trep=%lu trej=%lu resend=%lu off=%ldms"
                     " | cur=%lu gap=%ld dupe=%u late=%u lost=%u conceal=%u overwr=%u"
                     " | swq=%ld drv=%ld t=%lums resync=%lu skip=%lu respawn=%d\n",
                     st.recv, st.recv - last_pkts, st.started,
                     st.pops, st.decoded, st.decfail, st.sink_last,
                     st.sync, st.treq, st.trep, st.trej, st.resend, st.off_ms,
                     st.play_cursor, st.gap_ahead,
                     st.jdupe, st.jlate, st.jlost, st.jconceal, st.joverwr,
                     st.sw_queued, st.drv_delay, st.mono_ms,
                     st.resync, st.skipped, st.respawns);
                last_pkts = st.recv;
            }
            continue;
        }

        if (pfd[0].revents & POLLIN) on_listen_readable(S);
        if (nfd == 2 && S->client_fd >= 0 &&
            (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))
            on_client_readable(S);
    }
}

/* Resolve the primary non-loopback IPv4 (+ its MAC on Linux). Returns 0 on
 * success. ip4[] is the dotted-quad in order; ip_be is network byte order. */
static int resolve_primary(uint8_t ip4[4], uint8_t mac[6], uint32_t *ip_be) {
    struct ifaddrs *ifa = NULL, *p;
    if (getifaddrs(&ifa) != 0) return -1;
    const char *want = getenv("AIRPLAY_IFACE");    /* exact-name override (bringup) */
    char ifname[IF_NAMESIZE] = ""; int found = 0;
    /* Prefer a real-LAN interface (Wi-Fi/eth) over the usbnet gadget: usb0 is a
     * Mac-only point-to-point link a Wi-Fi sender (iPhone) can't reach, so picking
     * it first (getifaddrs order) makes the advert unreachable off the tether.
     * name heuristic ("usb*" = gadget); AIRPLAY_IFACE forces a specific
     * iface if the heuristic ever guesses wrong. */
    int have_chosen = 0, have_fallback = 0;
    uint32_t chosen = 0, fallback = 0;
    char chosen_name[IF_NAMESIZE] = "", fallback_name[IF_NAMESIZE] = "";
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr.s_addr;
        if (want) {                            /* honor the override exactly, or fail */
            if (strcmp(p->ifa_name, want) == 0) {
                chosen = a; snprintf(chosen_name, sizeof chosen_name, "%s", p->ifa_name);
                have_chosen = 1; break;
            }
            continue;
        }
        if (strncmp(p->ifa_name, "usb", 3) != 0) {
            if (!have_chosen) { chosen = a;
                snprintf(chosen_name, sizeof chosen_name, "%s", p->ifa_name); have_chosen = 1; }
        } else if (!have_fallback) { fallback = a;
            snprintf(fallback_name, sizeof fallback_name, "%s", p->ifa_name); have_fallback = 1; }
    }
    if (have_chosen)        { memcpy(ip4, &chosen, 4);   *ip_be = chosen;
        snprintf(ifname, sizeof ifname, "%s", chosen_name);   found = 1; }
    else if (have_fallback) { memcpy(ip4, &fallback, 4); *ip_be = fallback;
        snprintf(ifname, sizeof ifname, "%s", fallback_name); found = 1; }
    memset(mac, 0, 6);
#if defined(__linux__)
    if (found) {
        for (p = ifa; p; p = p->ifa_next) {
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_PACKET &&
                strcmp(p->ifa_name, ifname) == 0) {
                struct sockaddr_ll *ll = (struct sockaddr_ll *)(void *)p->ifa_addr;
                if (ll->sll_halen == 6) memcpy(mac, ll->sll_addr, 6);
                break;
            }
        }
    }
#endif
    freeifaddrs(ifa);
    return found ? 0 : -1;
}

/* Classic double-fork daemonize. Returns 0 in the surviving daemon. */
static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);
    if (setsid() < 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);
    if (chdir("/") != 0) { /* non-fatal */ }
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("raopd %s\n", RAOPD_VERSION);
        return 0;
    }

    int smoke = (argc == 3 && strcmp(argv[1], "--smoke") == 0);
    g_fg = (argc == 2 && (strcmp(argv[1], "--foreground") == 0 ||
                          strcmp(argv[1], "-f") == 0));

    signal(SIGPIPE, SIG_IGN);       /* a dropped client must not kill us */
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    server_t S; memset(&S, 0, sizeof S);
    S.client_fd = -1;
    { const char *pfx = getenv("AIRPLAY_PREFIX");
      snprintf(S.prefix, sizeof S.prefix, "%s", pfx ? pfx : "/var/local/shairkindle"); }
    dacp_state_init();

    uint32_t ip_be;
    int mdns_up = 0;

    if (smoke) {
        int port = atoi(argv[2]);
        if (port <= 0) { fprintf(stderr, "raopd: bad --smoke port\n"); return 1; }
        /* loopback, dummy identity; the smoke probes never reach RECORD/mDNS. */
        S.ip4[0] = 127; S.ip4[1] = 0; S.ip4[2] = 0; S.ip4[3] = 1;
        memset(S.mac, 0, 6);
        ip_be = htonl(INADDR_LOOPBACK);
        S.listen_fd = rtsp_listen(ip_be, port);
        if (S.listen_fd < 0) { perror("rtsp_listen"); return 1; }
    } else {
        if (!g_fg && daemonize() != 0) return 1;

        const char *prefix = getenv("AIRPLAY_PREFIX");
        if (!prefix) prefix = "/var/local/shairkindle";
        char lockp[512], pidp[512];
        snprintf(lockp, sizeof lockp, "%s/raopd.lock", prefix);
        snprintf(pidp, sizeof pidp, "%s/raopd.pid", prefix);
        if (raopd_lock_acquire(lockp) < 0) return 1;   /* another instance holds it */
        raopd_pid_publish(pidp, getpid());

        /* At boot the daemon can start before the Wi-Fi interface has associated,
         * so don't give up on the first miss — wait (bounded) for a primary
         * interface to come UP. Interruptible: a SIGTERM during the wait sets
         * g_stop and breaks the loop. On give-up, remove our just-published
         * pidfile so it isn't left stale (the flock already auto-releases). */
        int resolved = 0;
        for (int i = 0; i < IFACE_WAIT_TRIES && !g_stop; i++) {
            if (resolve_primary(S.ip4, S.mac, &ip_be) == 0) { resolved = 1; break; }
            sleep(IFACE_WAIT_SECS);
        }
        if (!resolved) { unlink(pidp); return 1; }
        LOGF("[raopd] interface up: %u.%u.%u.%u  mac %02x:%02x:%02x:%02x:%02x:%02x\n",
             S.ip4[0], S.ip4[1], S.ip4[2], S.ip4[3],
             S.mac[0], S.mac[1], S.mac[2], S.mac[3], S.mac[4], S.mac[5]);

        char txt[512];
        /* RAOP service instance MUST be "<12-hex-MAC>@<name>" — iOS's AirPlay
         * picker parses the device id from it and ignores _raop._tcp services
         * that lack the prefix (a raw mDNS browse doesn't care). */
        char apname[RAOP_NAME_MAX_BYTES + 1];
        raop_name_sanitize(getenv("AIRPLAY_NAME"), apname, sizeof apname);
        setenv("AIRPLAY_NAME", apname, 1);   /* renderer (via environ) shows == advertised */
        char instance[96];
        snprintf(instance, sizeof instance, "%02X%02X%02X%02X%02X%02X@%s",
                 S.mac[0], S.mac[1], S.mac[2], S.mac[3], S.mac[4], S.mac[5], apname);
        if (raop_build_txt(txt, sizeof txt, NULL, 0) > 0 &&
            raop_mdns_start(instance, RTSP_PORT, txt, ip_be) == 0)
            mdns_up = 1;
        LOGF("[raopd] mDNS %s as \"%s\"; listening on 0.0.0.0:%d\n",
             mdns_up ? "up" : "FAILED", instance, RTSP_PORT);

        S.listen_fd = rtsp_listen(htonl(INADDR_ANY), RTSP_PORT);
        if (S.listen_fd < 0) { unlink(pidp); return 1; }
    }

    /* Start the render worker only after we own the lock + have an interface +
       are listening (so a losing instance / failed bringup never paints). In
       --smoke, run the worker (so wake-wiring tests still exercise it) but do
       NOT paint on start (no real fbink on a host; no accidental device paint). */
    if (nprender_start(S.prefix, !smoke) != 0)
        LOGF("[raopd] nprender_start FAILED (no display)\n");

    rtsp_session_init(&S.rtsp, S.ip4, S.mac, AUDIO_LATENCY);
    S.iface_ip_be = ip_be;              /* mDNS egress iface for DACP resolution */

    /* DACP control thread: relays button tokens (kindlet -> supervisor -> here)
     * to DACP commands to the sender. Daemon path only (needs prefix + iface). */
    dacp_ctl_cfg_t ctl_cfg; memset(&ctl_cfg, 0, sizeof ctl_cfg);
    volatile int ctl_stop = 0;
    pthread_t ctl_th; int ctl_started = 0;
    if (!smoke) {
        snprintf(ctl_cfg.prefix, sizeof ctl_cfg.prefix, "%s", S.prefix);
        ctl_cfg.iface_ip_be = S.iface_ip_be;
        ctl_cfg.stop = &ctl_stop;
        if (pthread_create(&ctl_th, NULL, dacp_control_thread, &ctl_cfg) == 0) ctl_started = 1;
        else LOGF("[raopd] DACP control thread failed to start\n");
    }

    run_loop(&S);

    if (ctl_started) { ctl_stop = 1; pthread_join(ctl_th, NULL); }

    /* clean shutdown. Stop the render worker FIRST: teardown_client() calls
       np_set_playing(0)+nprender_wake(), which on the shutdown path would
       dispatch a farewell "waiting" splash that races the framework's Home
       repaint and orphans over Home (spec: no farewell splash on shutdown).
       nprender_stop() joins the worker, so the later wake is a no-op. */
    nprender_stop();
    teardown_client(&S);
    if (mdns_up) raop_mdns_stop();
    if (S.listen_fd >= 0) close(S.listen_fd);
    if (!smoke) {
        char pidp2[512];
        snprintf(pidp2, sizeof pidp2, "%s/raopd.pid", S.prefix);
        unlink(pidp2);
    }
    return 0;
}
