#include "sink.h"
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <math.h>

// ---- phone-volume software gain (shared by both backends) ----
// AirPlay's SET_PARAMETER volume slider is applied as an ATTENUATION-ONLY software gain
// on the decoded PCM here -- NOT as a hardware mixer level. This is orthogonal to volumd
// by design: volumd still owns the hardware output level (amixer), raopd just scales its
// own stream. Q8 fixed-point (0..256, 256=unity/0dB) so sink_write's hot path is a plain
// shift, no float math per-sample. _Atomic because sink_set_volume runs on the RTSP/main
// thread (main.c's ACT_SET_VOLUME) while sink_write reads it on the player thread.
static std::atomic<int> g_gain_q8{256};

extern "C" void sink_set_volume(int pct) {
    float gain;
    if (pct <= 0)        gain = 0.0f;   // mute
    else if (pct >= 100) gain = 1.0f;   // 0 dB, unity -- exact, no float roundoff drift
    else {
        // raop_volume_db_to_pct maps dB -> pct LINEARLY (pct=(db+30)/30*100); invert it
        // back to dB here and apply the perceptual amplitude curve (10^(dB/20)) so the
        // slider's midpoint sounds like half-loudness rather than half-amplitude.
        float db = (pct / 100.0f) * 30.0f - 30.0f;
        gain = powf(10.0f, db / 20.0f);
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;   // attenuation-only: never amplify past the source
    g_gain_q8.store((int)(gain * 256.0f + 0.5f), std::memory_order_relaxed);
}

static void apply_gain(int16_t *dst, const int16_t *src, int n_samples) {
    int g = g_gain_q8.load(std::memory_order_relaxed);
    for (int i = 0; i < n_samples; i++) {
        dst[i] = (int16_t)(((int32_t)src[i] * g) >> 8);
    }
}

#ifdef SINK_RECORD_BACKEND
// Host session-test backend: record PCM in-process (mirrors the old fake_kkbaudio).
extern "C" {
int16_t sink_rec_buf[16384];
size_t  sink_rec_n = 0;
size_t  sink_rec_max_accept = (size_t)-1;   // scriptable partial-accept
int     sink_rec_write_calls = 0;
int     sink_rec_flushed = 0;
}
extern "C" int  sink_open(int, int){ sink_rec_n = 0; return 0; }
extern "C" int  sink_write(const int16_t* pcm, int n){
    sink_rec_write_calls++;
    size_t accept = (size_t)n < sink_rec_max_accept ? (size_t)n : sink_rec_max_accept;
    if (sink_rec_n + accept <= sizeof(sink_rec_buf)/sizeof(sink_rec_buf[0])){
        apply_gain(sink_rec_buf + sink_rec_n, pcm, (int)accept); sink_rec_n += accept;
    }
    return (int)accept;
}
extern "C" long sink_driver_delay_frames(void){ return 3; }
extern "C" long sink_queued_frames(void){ return 8; }
extern "C" long sink_pending_bytes(void){ return 0; }
extern "C" int  sink_respawns(void){ return 0; }
extern "C" int  sink_child_alive(void){ return 1; }
extern "C" void sink_flush(void){ sink_rec_flushed = 1; sink_rec_n = 0; }
extern "C" void sink_close(void){ }
#else  // ---- aplay-pipe backend (device + test_sink_aplay) ----
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

extern char **environ;

// g_wfd/g_pid/g_respawns are pl_thread-only: child_alive() reaps via waitpid as a
// side effect and mutates g_pid/g_wfd, so any other thread touching these (or calling
// child_alive()/sink_child_alive()) would race pl_thread. session_get_stats() honors
// this by calling only the pure readers (sink_respawns/sink_queued_frames/
// sink_driver_delay_frames), never sink_child_alive().
static int   g_wfd = -1;         // write end of pipe (non-blocking)
static pid_t g_pid = -1;
static int   g_respawns = 0;         // successful respawns (diagnostic, sink_respawns())
static int   g_spawn_attempts = 0;    // spawn_child() ATTEMPTS since last reset -- caps a
                                       // persistently-failing spawn (see sink_write); a failed
                                       // posix_spawn must count here too, or the cap never bites
static const int WRITE_WAIT_MS = 200;

static void build_argv(char* argv[], int max){
    const char* env = getenv("RAOPD_APLAY");
    static char buf[256];
    const char* def = "/usr/bin/aplay -q -D plug:dmix0 -f S16_LE -c 2 -r 44100 -t raw -";
    snprintf(buf, sizeof buf, "%s", env ? env : def);
    int n=0; char* save=nullptr;
    for(char* t=strtok_r(buf," ",&save); t && n<max-1; t=strtok_r(nullptr," ",&save)) argv[n++]=t;
    argv[n]=nullptr;
}

static void reap(void);

// Bounded terminate: SIGTERM + grace, then SIGKILL + another grace. A D-state
// (uninterruptible kernel sleep) child can't be preempted even by SIGKILL, so this never
// blocks its caller -- a truly-stuck child leaks as an orphan/zombie for the kernel to
// reap later. A leaked orphan beats a hung caller. Shared by reap() (the long-lived sink
// child) and spawn_child()'s O_NONBLOCK-failure path (a child that never made it into g_pid).
static void kill_bounded(pid_t pid){
    kill(pid, SIGTERM);
    int reaped = 0;
    for (int i = 0; i < 20 && !reaped; i++) {
        pid_t r;
        do { r = waitpid(pid, nullptr, WNOHANG); } while (r < 0 && errno == EINTR);
        if (r == pid || (r < 0 && errno == ECHILD)) { reaped = 1; break; }  // reaped / already gone
        usleep(5000);
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        for (int i = 0; i < 20 && waitpid(pid, nullptr, WNOHANG) == 0; i++) usleep(5000);
    }
}

static int spawn_child(void){
    if (g_pid > 0) reap();   // guard against double-spawn leaking the old child's fd/proc
    char* argv[32]; build_argv(argv, 32);
    if (!argv[0]) return -1;   // empty/all-whitespace RAOPD_APLAY -- posix_spawn(NULL) is UB
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);      // child stdin <- pipe read
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]);
    if (rc != 0) { close(fds[1]); return -1; }
    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
        // Without O_NONBLOCK, sink_write's write() could block unboundedly on a full pipe --
        // the poll-budget backpressure logic can't help, write() itself blocks. Treat this
        // exactly like a failed spawn: kill the child we just started and report failure.
        close(fds[1]);
        kill_bounded(pid);
        return -1;
    }
    g_wfd = fds[1]; g_pid = pid;
    return 0;
}

// pl_thread-only -- reaps via waitpid (mutates g_pid/g_wfd as a side effect), so
// calling this (or sink_child_alive()) from the stats/main thread would race pl_thread.
static int child_alive(void){
    if (g_pid <= 0) return 0;
    int st; pid_t r = waitpid(g_pid, &st, WNOHANG);
    if (r == g_pid) { g_pid = -1; if (g_wfd>=0){ close(g_wfd); g_wfd=-1; } return 0; }
    return 1;
}

static void reap(void){
    if (g_wfd >= 0){ close(g_wfd); g_wfd = -1; }
    if (g_pid > 0){ kill_bounded(g_pid); g_pid = -1; }
}

extern "C" int sink_open(int, int){
    static int siginit=0; if(!siginit){ signal(SIGPIPE, SIG_IGN); siginit=1; }
    g_respawns = 0;
    g_spawn_attempts = 0;
    int rc = spawn_child();
    /* Best-effort: clear WM8960 output mute without forcing a level (volumd owns level).
       Device only -- host tests set RAOPD_APLAY and skip this. Backgrounded (trailing `&`,
       whole pipeline in a subshell so both amixer calls background as one job): system()
       returns right after the fork, so a hung ALSA/shell can't block sink_open -- and
       sink_open runs on the RTSP RECORD path, before any player thread exists. Confirmed
       remotely on this K3: Speaker/Headphone are volume-only controls with no mute switch,
       so this is currently a no-op; whether to keep it is a product call deferred to an
       in-person audibility test -- this fix only removes the unbounded-hang risk. */
    if (!getenv("RAOPD_APLAY")) {
        if (system("(amixer -q sset 'Speaker' unmute >/dev/null 2>&1; "
                   "amixer -q sset 'Headphone' unmute >/dev/null 2>&1) &") == -1) { /* ignore */ }
    }
    return rc;
}

extern "C" int sink_write(const int16_t* pcm, int n_samples){
    if (n_samples <= 0) return 0;
    if (!child_alive()) {
        // Cap ATTEMPTS, not successes: on musl, posix_spawn reports a missing/misnamed
        // aplay as a FAILURE (spawn_child returns -1), so counting only successes would
        // never trip the cap -- every sink_write would retry pipe()+posix_spawn() forever
        // (a fork/CPU storm). g_spawn_attempts counts every try; g_spawn_attempts >= 1000
        // means "reset via sink_open/sink_flush before spawning is attempted again".
        if (g_spawn_attempts >= 1000) return 0;
        g_spawn_attempts++;
        if (spawn_child() == 0) g_respawns++; else return 0;
    }
    // pcm is const (can't gain-scale in place), so scale into a bounded local buffer and
    // write THAT to the pipe. Chunked through GAIN_BUF_N to bound the stack regardless of
    // caller n_samples (SDP allows frame_length up to 16384; canonical AirPlay is 352
    // frames/704 stereo samples per period, well under one chunk).
    static const int GAIN_BUF_N = 2048;
    int16_t gbuf[GAIN_BUF_N];
    int total_accepted = 0;
    int waited = 0;
    while (total_accepted < n_samples){
        int chunk = n_samples - total_accepted;
        if (chunk > GAIN_BUF_N) chunk = GAIN_BUF_N;
        apply_gain(gbuf, pcm + total_accepted, chunk);

        const char* p = (const char*)gbuf;
        size_t total = (size_t)chunk * 2, off = 0;
        int stop = 0;
        while (off < total){
            ssize_t w = write(g_wfd, p + off, total - off);
            // NOTE: `waited` is intentionally NOT reset here (nor across chunks). It must
            // be a cumulative bound on total blocking time across the whole call, not just
            // the zero-progress case -- otherwise a slowly-but-continuously-draining child
            // (CPU starvation, dmix contention) could hold this call for a time proportional
            // to n_samples with no absolute ceiling.
            if (w > 0){ off += (size_t)w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                if (waited >= WRITE_WAIT_MS) { stop = 1; break; }   // persistent-full => drop remainder
                struct pollfd pfd = { g_wfd, POLLOUT, 0 };
                poll(&pfd, 1, 20); waited += 20;
                if (!child_alive()) { stop = 1; break; }
                continue;
            }
            stop = 1; break;                                        // EPIPE/other => child died; drop
        }
        total_accepted += (int)(off / 2);
        if (stop) break;
    }
    return total_accepted;                                    // accepted SAMPLES
}

extern "C" void sink_flush(void){ reap(); g_spawn_attempts = 0; spawn_child(); }  // restart: buffered
                                                               // bytes can't be recalled; also re-arms
                                                               // the spawn-attempt cap for the new epoch
extern "C" void sink_close(void){ reap(); }
extern "C" long sink_driver_delay_frames(void){ return -1; }
extern "C" long sink_queued_frames(void){ return -1; }
extern "C" long sink_pending_bytes(void){ return -1; }        // not observable on the write end
extern "C" int  sink_respawns(void){ return g_respawns; }
extern "C" int  sink_child_alive(void){ return child_alive(); }  // pl_thread-only, see child_alive()
#endif
