/* src/dacp.c — outbound DACP request formatting, socket sender, and local
 * control thread. See src/dacp.h. */
#include "dacp.h"
#include "dacp_state.h"
#include "dacp_resolve.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/times.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *cmd_path(dacp_cmd_t c) {
    switch (c) {
        case DACP_PLAYPAUSE: return "playpause";
        case DACP_NEXTITEM:  return "nextitem";
        case DACP_PREVITEM:  return "previtem";
        default:             return NULL;
    }
}

int dacp_format_request(char *buf, size_t cap, dacp_cmd_t cmd,
                        const char *host, unsigned port, const char *token) {
    const char *path = cmd_path(cmd);
    if (!path || !host || !token) return -1;
    if (port < 1 || port > 65535) return -1;
    if (strlen(host) > 45 || strlen(token) > 31) return -1;   /* IPv4/46 ceiling; token cap */
    int n = snprintf(buf, cap,
        "GET /ctrl-int/1/%s HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "Active-Remote: %s\r\n"
        "Connection: close\r\n\r\n",
        path, host, port, token);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

long mono_ms(void) {
    static long hz = 0;
    if (hz <= 0) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    struct tms tb;
    clock_t t = times(&tb);
    return (long)((long long)t * 1000 / hz);
}

int dacp_debounce_ok(dacp_cmd_t cmd, long now_ms, long window_ms) {
    static long last[3] = { -1000000, -1000000, -1000000 };
    int i = (int)cmd;
    if (i < 0 || i > 2) return 0;
    if (last[i] != -1000000 && (now_ms - last[i]) < window_ms) return 0;
    last[i] = now_ms;
    return 1;
}

/* --- socket sender + control thread (device path) --- */

/* Non-blocking connect to ip_be:port with a bounded poll timeout. Returns a
 * connected blocking fd, or -1. */
static int tcp_connect_timeout(unsigned ip_be, unsigned port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_be;
    sa.sin_port = htons((unsigned short)port);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r != 0) {
        if (errno != EINPROGRESS) { close(fd); return -1; }
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) <= 0) { close(fd); return -1; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);      /* back to blocking for the tiny write/read */
    return fd;
}

/* Parse the numeric status out of "HTTP/1.x NNN ...". Returns NNN or -1. */
static int parse_http_status(const char *buf, size_t n) {
    /* find the first space, then read 3 digits */
    size_t i = 0;
    while (i < n && buf[i] != ' ') i++;
    if (i + 4 > n) return -1;
    i++;
    if (buf[i] < '0' || buf[i] > '9') return -1;
    int code = 0;
    for (int k = 0; k < 3 && i < n && buf[i] >= '0' && buf[i] <= '9'; k++, i++)
        code = code * 10 + (buf[i] - '0');
    return code;
}

int dacp_send_command(dacp_cmd_t cmd, unsigned iface_ip_be) {
    const char *cname = cmd_path(cmd);
    dacp_snapshot_t snap;
    dacp_state_snapshot(&snap);
    if (!snap.have_creds) { fprintf(stderr, "[DACP] no creds; cmd=%s ignored\n", cname ? cname : "?"); return -1; }

    char owner[128];
    snprintf(owner, sizeof owner, "iTunes_Ctrl_%s._dacp._tcp.local", snap.dacp_id);

    unsigned port = (snap.resolved_gen == snap.generation) ? snap.resolved_port : 0;
    if (!port) {
        long t0 = mono_ms();
        port = dacp_resolve_port(owner, iface_ip_be, mono_ms() + 2000);
        if (!port) { fprintf(stderr, "[DACP] srv-query %s -> no port (%ldms)\n", owner, mono_ms() - t0); return -1; }
        dacp_state_publish_port(snap.generation, port);
        fprintf(stderr, "[DACP] srv-resolved port=%u\n", port);
    }

    /* Final guard right before we touch the network: re-snapshot and confirm the
     * session + creds + peer still match what we resolved for. Drops a command
     * that a session rollover (new sender) has invalidated. This closes all but
     * the tiny connect+write window; a strict guarantee would hold the
     * state lock across the blocking network I/O, which we won't do. */
    {
        dacp_snapshot_t cur; dacp_state_snapshot(&cur);
        if (!cur.have_creds || cur.generation != snap.generation ||
            cur.peer_ip_be != snap.peer_ip_be ||
            strcmp(cur.dacp_id, snap.dacp_id) != 0 ||
            strcmp(cur.active_remote, snap.active_remote) != 0) {
            fprintf(stderr, "[DACP] session/creds changed before send; drop cmd=%s\n", cname);
            return -1;
        }
        snap = cur;
    }

    struct in_addr ia; ia.s_addr = snap.peer_ip_be;
    char host[16]; snprintf(host, sizeof host, "%s", inet_ntoa(ia));
    char req[256];
    int rn = dacp_format_request(req, sizeof req, cmd, host, port, snap.active_remote);
    if (rn < 0) { fprintf(stderr, "[DACP] format failed cmd=%s\n", cname); return -1; }

    long t0 = mono_ms();
    int fd = tcp_connect_timeout(snap.peer_ip_be, port, 1000);
    if (fd < 0) { fprintf(stderr, "[DACP] cmd=%s host=%s:%u connect=fail\n", cname, host, port); return -1; }

    int code = -1;
    /* bounded write */
    ssize_t w = write(fd, req, (size_t)rn);
    if (w == rn) {
        /* bounded read of the status line (poll with a small remaining budget) */
        char resp[128];
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) > 0) {
            ssize_t rr = read(fd, resp, sizeof resp - 1);
            if (rr > 0) { resp[rr] = 0; code = parse_http_status(resp, (size_t)rr); }
        }
    }
    close(fd);
    fprintf(stderr, "[DACP] cmd=%s host=%s:%u http=%d elapsed=%ldms\n",
            cname, host, port, code, mono_ms() - t0);
    return code;
}

void *dacp_control_thread(void *arg) {
    dacp_ctl_cfg_t *cfg = (dacp_ctl_cfg_t *)arg;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { fprintf(stderr, "[DACP] control socket() failed: %s\n", strerror(errno)); return NULL; }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    char path[300];
    snprintf(path, sizeof path, "%s/control.sock", cfg->prefix);
    struct sockaddr_un un = {0};
    un.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof un.sun_path) {
        fprintf(stderr, "[DACP] control path too long: %s\n", path); close(fd); return NULL;
    }
    strcpy(un.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&un, sizeof un) != 0) {
        fprintf(stderr, "[DACP] bind %s: %s\n", path, strerror(errno)); close(fd); return NULL;
    }
    chmod(path, 0600);
    fprintf(stderr, "[DACP] control thread ready at %s\n", path);

    while (!*cfg->stop) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 250) <= 0) continue;
        if (!(p.revents & POLLIN)) continue;   /* POLLERR/HUP/NVAL: don't blocking-recv */
        char buf[64];
        ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (n <= 0) continue;
        buf[n] = 0;
        char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
        dacp_cmd_t cmd;
        if      (!strcmp(buf, "NEXT"))      cmd = DACP_NEXTITEM;
        else if (!strcmp(buf, "PREV"))      cmd = DACP_PREVITEM;
        else if (!strcmp(buf, "PLAYPAUSE")) cmd = DACP_PLAYPAUSE;
        else continue;
        if (dacp_debounce_ok(cmd, mono_ms(), 300))
            dacp_send_command(cmd, cfg->iface_ip_be);
    }
    unlink(path);
    close(fd);
    return NULL;
}
