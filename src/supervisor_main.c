/* airplay-supervisor: bind 127.0.0.1:SUP_PORT, accept one owner (the kindlet),
 * and run the existing airplay-on/off scripts on lease start/stop. The live
 * socket is the lease. Reuses the proven shell sequence (staging, wifi power,
 * firewall, TERM->wait->KILL) rather than reimplementing it here.
 *
 * shells out via the scripts (fork/execl, not system()) instead of
 * owning raopd's PID directly. Ceiling: teardown goes through airplay-off's
 * pidfile after verifying /proc/<pid>/exe. Upgrade path if the
 * pidfile indirection proves racy: make the supervisor fork/exec raopd as its
 * own child and reap it. */
#include "supervisor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/times.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define TICK_MS 5000

/* A launched-but-never-connected supervisor self-exits after this many ms of
 * waiting on accept() (the heartbeat timeout only covers an ACTIVE lease).
 * Env-overridable for tests. */
static int prelease_ms(void) {
    const char *e = getenv("SUP_PRELEASE_MS");
    if (e && *e) { int v = atoi(e); if (v > 0) return v; }
    return 60000;
}

/* Best-effort relay of a DACP command token to raopd's control socket. The
 * sandboxed kindlet can only reach us (5566); raopd holds the DACP creds and
 * does the mDNS/HTTP. sendto is connectionless: if raopd is down between
 * sessions the datagram is dropped (ENOENT/ECONNREFUSED) and the lease is
 * unaffected. The fd is opened once and reused. */
static void relay_cmd(const char *cmd) {
    static int fd = -1;
    if (fd < 0) {
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    const char *prefix = getenv("AIRPLAY_PREFIX");
    if (!prefix) prefix = "/var/local/shairkindle";
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    if ((size_t)snprintf(un.sun_path, sizeof un.sun_path, "%s/control.sock", prefix) >= sizeof un.sun_path)
        return;
    char line[32];
    int ln = snprintf(line, sizeof line, "%s\n", cmd);
    if (ln < 0) return;
    if (sendto(fd, line, (size_t)ln, 0, (struct sockaddr *)&un, sizeof un) < 0) {
        if (errno != ENOENT && errno != ECONNREFUSED)   /* raopd-down is expected, don't spam */
            fprintf(stderr, "[supervisor] relay %s: %s\n", cmd, strerror(errno));
    } else {
        fprintf(stderr, "[supervisor] relay %s\n", cmd);    /* one line per button; low volume */
    }
}

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig){ (void)sig; g_stop = 1; }

/* Monotonic milliseconds via times() -- a plain syscall with NO vDSO probe.
 *
 * We deliberately AVOID libc clock_gettime here: zig's musl runs a one-time
 * vDSO detection (cgt_init) on the first clock_gettime, which faults on this
 * pre-vDSO ARM kernel (2.6.26 -- ARM got a vDSO ~4.1): it derives a garbage
 * function pointer and STREXes it to kernel space -> layout-dependent SIGSEGV
 * (a heisenbug). The raw clock_gettime syscall dodges the crash but its ts is
 * left unfilled here (bad marshalling on this target -> garbage time). times()
 * is monotonic (ticks since boot), needs no vDSO, and marshals correctly.
 * clock_t wraps ~248d of uptime at 100Hz; negligible for a Kindle
 * that reboots/sleeps far sooner -- revisit only if long uptimes matter. */
static int64_t now_ms(void) {
    static long hz = 0;
    if (hz <= 0) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    struct tms tb;
    clock_t t = times(&tb);
    return (int64_t)t * 1000 / hz;
}

/* Run "<bindir>/<name>" via /bin/sh with the args passed directly (no shell
 * string -> no injection via AIRPLAY_BIN), BOUNDED by deadline_ms and, when
 * honor_stop is set, cancellable via g_stop. bindir defaults to the self-install
 * payload path (/var/local/shairkindle); override with AIRPLAY_BIN. Returns the
 * script's exit status, or -1 on fork/exec/length failure or if we had to kill it.
 *
 * Why bounded: the original blocked in waitpid() forever, so a hung foreground
 * command inside a script (iptables/wmiconfig, or a wedged wait) would pin the
 * whole supervisor -- no token relay, no heartbeat timeout, unkillable by
 * SIGTERM (the EINTR-retry swallowed it). That silently killed button relay in
 * the field. We now poll for exit; past the deadline, or (when honor_stop) as
 * soon as the supervisor is shutting down, we SIGKILL the DIRECT child only.
 * NOT a process-group kill: airplay-on backgrounds raopd in the same group, so a
 * group kill would take raopd down with the hung shell. A hung grandchild
 * (e.g. a stuck iptables) is left orphaned to init -- detached, no longer ours,
 * and it can't block us once the shell is reaped.
 *
 * honor_stop: the START path (airplay-on) passes 1 -- a wedged startup stays
 * cancellable by SIGTERM. The exit teardown (airplay-off) passes 0 -- SIGTERM
 * is what TRIGGERS teardown, so honoring g_stop there would self-cancel the
 * very script that closes the firewall/wifi/raopd; it must run to completion,
 * bounded only by deadline_ms. */
static int run_script(const char *name, int deadline_ms, int honor_stop) {
    const char *bindir = getenv("AIRPLAY_BIN");
    if (!bindir) bindir = "/var/local/shairkindle";
    char path[512];
    int k = snprintf(path, sizeof path, "%s/%s", bindir, name);
    if (k < 0 || (size_t)k >= sizeof path) {
        fprintf(stderr, "[supervisor] path too long: %s/%s\n", bindir, name);
        return -1;
    }
    fprintf(stderr, "[supervisor] run %s\n", path);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) { execl("/bin/sh", "sh", path, (char *)NULL); _exit(127); }

    int st = 0, killed = 0;
    int64_t start = now_ms();
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;                          /* reaped */
        if (r < 0) { if (errno == EINTR) continue; perror("waitpid"); return -1; }
        /* r == 0: still running */
        if (!killed && ((honor_stop && g_stop) || now_ms() - start > deadline_ms)) {
            fprintf(stderr, "[supervisor] %s %s after %lldms -> SIGKILL child\n",
                    name, (honor_stop && g_stop) ? "cancelled" : "TIMED OUT",
                    (long long)(now_ms() - start));
            kill(pid, SIGKILL);                       /* direct child only -> spares raopd */
            killed = 1;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };  /* poll every 50 ms */
        nanosleep(&ts, NULL);
    }
    if (killed) return -1;
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0) fprintf(stderr, "[supervisor] %s -> rc=%d\n", name, rc);
    return rc;
}

static const char *sup_prefix(void) {
    const char *p = getenv("AIRPLAY_PREFIX");
    return p ? p : "/var/local/shairkindle";
}

static unsigned long long self_starttime(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    fclose(f);
    unsigned long long st = 0;
    sup_stat_starttime(line, &st);
    return st;
}

/* Atomic (tmp+rename): the reader never sees a half-written pidfile. Returns 0 on
 * success, -1 on ANY failure. The pidfile is the registration boundary:
 * a supervisor that cannot publish a valid token is undiscoverable by synchronous
 * teardown, so main() ABORTS startup on failure rather than listening invisibly. */
static int write_pidfile(void) {
    const char *pre = sup_prefix();
    char path[512], tmp[512], buf[64];
    unsigned long long st = self_starttime();
    if (st == 0) return -1;                          /* no starttime token -> not discoverable */
    if ((size_t)snprintf(path, sizeof path, "%s/supervisor.pid", pre) >= sizeof path) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/supervisor.pid.tmp.%ld", pre, (long)getpid()) >= sizeof tmp) return -1;
    int n = sup_pidfile_format(buf, sizeof buf, (long)getpid(), st);
    if (n < 0) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    /* full-write loop: a single write() may be short even for a small regular file */
    int off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        off += (int)w;
    }
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void remove_pidfile(void) {
    const char *pre = sup_prefix();
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/supervisor.pid", pre) < sizeof path) unlink(path);
}

/* Teardown on ANY supervisor exit (STOP/EOF or heartbeat timeout). Runs to
 * completion (honor_stop=0) so a SIGTERM-driven exit still does fw_close +
 * wifi_powersave + raopd TERM/KILL + pidfile removal. NOT required to fit the
 * kindlet's ~800ms window: the kindlet's own group-KILL is the authoritative
 * ceiling on kindlet-driven paths; on the crash/heartbeat path (no kindlet)
 * this is raopd's SOLE killer, and there is no external deadline, so the
 * generous bound is fine. */
static int g_torn = 0;
static void supervisor_teardown(void) {
    if (g_torn) return;                    /* idempotent: session-end + one-shot final both call it */
    g_torn = 1;
    run_script("airplay-off", 12000, 0);   /* honor_stop=0: complete even under SIGTERM */
    remove_pidfile();
}

/* airplay-on's own worst case is ~9 s (8×1 s pidfile wait + 1 s control.sock);
 * airplay-off's is ~10 s (TERM + 10×1 s wait + KILL). Deadlines sit above those
 * so a legitimately-slow script is never killed, only a genuinely wedged one. */
static int do_action(sup_action_t a) {
    if (a == SUP_ACT_START) return run_script("airplay-on", 20000, 1);  /* cancellable */
    if (a == SUP_ACT_STOP)  { supervisor_teardown(); return 0; }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* sigaction WITHOUT SA_RESTART (not signal(), which sets SA_RESTART on Linux):
     * an idle supervisor is blocked in accept(); we need SIGTERM/SIGINT to
     * interrupt it with EINTR so the loop re-checks g_stop and shuts down,
     * instead of auto-restarting the syscall and ignoring the signal. */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Own a fresh session/process group so a single kill(-pgid,…) reaches the
     * whole subtree (airplay-on -> raopd -> render children). Abort if we can't:
     * without our own group the group-kill teardown invariant does not hold. */
    if (setsid() == -1) { perror("setsid"); return 1; }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 only */
    a.sin_port = htons(SUP_PORT);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
    if (listen(lfd, 1) != 0) { perror("listen"); return 1; }
    fcntl(lfd, F_SETFD, FD_CLOEXEC);
    fprintf(stderr, "[supervisor] listening on 127.0.0.1:%d\n", SUP_PORT);

    if (write_pidfile() != 0) {         /* only now: a real, bound, session-leader supervisor */
        perror("write_pidfile");
        close(lfd);
        return 1;
    }

    sup_state_t s; sup_state_init(&s);

    /* Startup reconciliation: a prior forced SIGKILL may have left the
     * firewall/wifi dirty. airplay-off (fw_close + wifi_powersave) + airplay-on's
     * unconditional wifi reconcile (Step 3) make every launch self-healing. */
    run_script("airplay-off", 12000, 0);

    /* Serve ONE kindlet session, then tear down and exit -- no lazy residency.
     * The kindlet's stop() now guarantees we are fully gone before the next launch,
     * so a fresh (freshly-installed) supervisor runs every time. */

    /* Pre-lease timeout: a launched-but-never-connected supervisor self-exits
     * (the heartbeat timeout only covers an ACTIVE lease). Poll the listener; if
     * nobody connects in time, skip accept and fall through to teardown. */
    if (!g_stop) {
        struct pollfd lp; lp.fd = lfd; lp.events = POLLIN; lp.revents = 0;
        int lpr = poll(&lp, 1, prelease_ms());
        if (lpr <= 0) {
            if (lpr == 0) fprintf(stderr, "[supervisor] no client within pre-lease timeout; exiting\n");
            g_stop = 1;
        }
    }

    int cfd = -1;
    if (!g_stop) {
        struct sockaddr_in cli; socklen_t cl = sizeof cli;
        cfd = accept(lfd, (struct sockaddr *)&cli, &cl);
        if (cfd < 0) { if (errno != EINTR) perror("accept"); }
        else fcntl(cfd, F_SETFD, FD_CLOEXEC);
    }

    if (cfd >= 0) {
        if (do_action(sup_step(&s, SUP_EV_ACCEPT, now_ms())) != 0) {
            /* airplay-on failed: don't leave an active lease with no receiver */
            fprintf(stderr, "[supervisor] airplay-on failed; tearing down\n");
            sup_step(&s, SUP_EV_STOP, now_ms());
            do_action(SUP_ACT_STOP);
            close(cfd);
        } else {
            sup_lb_t lb; sup_lb_init(&lb);
            int active = 1;
            while (active && !g_stop) {
                /* Check the heartbeat timeout every iteration, not only on poll
                 * timeout -- a peer that keeps the socket readable with junk must
                 * not be able to hold the lease open past the timeout. */
                if (sup_step(&s, SUP_EV_TICK, now_ms()) == SUP_ACT_STOP) {
                    do_action(SUP_ACT_STOP); active = 0; break;
                }
                struct pollfd p; p.fd = cfd; p.events = POLLIN; p.revents = 0;
                int pr = poll(&p, 1, TICK_MS);
                if (pr < 0) { if (errno == EINTR) continue; perror("poll"); break; }
                if (pr == 0) continue;   /* timeout -> loop; tick re-checked at top */
                if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    if (sup_step(&s, SUP_EV_EOF, now_ms()) == SUP_ACT_STOP) do_action(SUP_ACT_STOP);
                    active = 0; continue;
                }
                char rb[256];
                ssize_t nr = read(cfd, rb, sizeof rb);
                if (nr < 0) { if (errno == EINTR) continue;      /* real error == loss */
                    if (sup_step(&s, SUP_EV_EOF, now_ms()) == SUP_ACT_STOP) do_action(SUP_ACT_STOP);
                    active = 0; continue;
                }
                if (nr == 0) {                                   /* clean EOF */
                    if (sup_step(&s, SUP_EV_EOF, now_ms()) == SUP_ACT_STOP) do_action(SUP_ACT_STOP);
                    active = 0; continue;
                }
                sup_event_t evs[256];   /* worst case: 256 bytes of "\n" = 256 empty lines */
                int ne = sup_lb_push(&lb, rb, (size_t)nr, evs, 256);
                int i;
                for (i = 0; i < ne && active; i++) {
                    /* Relay DACP command tokens to raopd (they also refresh the
                     * lease heartbeat via sup_step below). */
                    switch (evs[i]) {
                        case SUP_EV_NEXT:      relay_cmd("NEXT");      break;
                        case SUP_EV_PREV:      relay_cmd("PREV");      break;
                        case SUP_EV_PLAYPAUSE: relay_cmd("PLAYPAUSE"); break;
                        default: break;
                    }
                    if (sup_step(&s, evs[i], now_ms()) == SUP_ACT_STOP) {
                        do_action(SUP_ACT_STOP); active = 0;
                    }
                }
            }
            close(cfd);
            /* ensure teardown even if the loop exited via g_stop mid-session */
            if (s.phase == SUP_ACTIVE) { sup_step(&s, SUP_EV_STOP, now_ms()); do_action(SUP_ACT_STOP); }
        }
    }

    supervisor_teardown();   /* idempotent: no-op if a session already tore down */
    close(lfd);
    return 0;
}
