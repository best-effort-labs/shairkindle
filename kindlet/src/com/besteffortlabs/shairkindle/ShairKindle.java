package com.besteffortlabs.shairkindle;

import com.besteffortlabs.kindletshell.*;

import java.awt.Component;
import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * shairkindle now-playing kindlet -- thin {@link KindletShell} subclass.
 *
 * Supplies the native payload (the airplay supervisor, which binds the lease
 * port and forks raopd), the wire encoder, and a draw-nothing display (raopd
 * paints the now-playing card via fbink underneath). All lifecycle, install,
 * launch, and lease-connect logic lives in {@link KindletShell}.
 */
public class ShairKindle extends KindletShell {

    public PayloadSpec payload() {
        // supervisor binds 5566 (the lease) and forks raopd; values match the old
        // NowPlayingShell.ensureSupervisor() invocation (verified).
        return new PayloadSpec(
            "/var/local/shairkindle", 5566, 10000,
            "airplay-supervisor", new String[0], "supervisor.log", "airplay-super",
            "/var/local/shairkindle/supervisor.pid");
    }

    public Encoder encoder() { return new NowPlayingEncoder(); }

    public Component display() {
        return new Component() { public void paint(java.awt.Graphics g) {} };
    }

    public void onLeaseState(boolean up) {
        try { getContext().setSubTitle(up ? "AirPlay ready" : "AirPlay off"); } catch (Throwable t) {}
    }

    /**
     * Synchronous teardown: TERM the supervisor's whole process GROUP,
     * poll until it is gone, KILL it past the deadline. Timing lives here (Java
     * Thread.sleep), never in shell because BusyBox sleep has no reliable sub-second
     * granularity. Validation lives in airplay-teardown-resolve (killing the wrong
     * group as root is the real hazard).
     */
    public void teardownAndWait(long deadlineMs) {
        long start = System.currentTimeMillis();      // clock starts BEFORE resolve
        String pid = resolveSupervisorPid();
        if (pid == null || pid.length() == 0) return;
        signalGroup(pid, "TERM");
        while (System.currentTimeMillis() - start < deadlineMs) {
            if (!groupAlive(pid)) return;        // ESRCH -> whole group gone
            long step = sleepStep();
            if (step > 0) { try { Thread.sleep(step); } catch (InterruptedException ie) { return; } }
        }
        signalGroup(pid, "KILL");                // best-effort final guarantee
    }

    /**
     * teardownAndWait threw before finishing -- do one VALIDATED group-KILL so the
     * supervisor dies within stop() rather than only via its lease-EOF self-exit. Reuses
     * the same resolve (exe + PID==PGID==SID; never signal an unvalidated group)
     * and bounded signal helpers, both of which swallow their own Throwables.
     */
    public void teardownFallback() {
        String pid = resolveSupervisorPid();
        if (pid != null && pid.length() > 0) signalGroup(pid, "KILL");
    }

    /** ms between group-alive polls. */
    protected long sleepStep() { return 50L; }
    /** Hard per-helper-exec cap so a wedged /bin/sh/kill can never hang stop(). */
    private static final long EXEC_CAP_MS = 300L;

    /** Print+return the validated supervisor group-leader pid ("" if none). */
    protected String resolveSupervisorPid() {
        return firstLine(new String[]{"/bin/sh",
            payload().installDir() + "/airplay-teardown-resolve"});
    }

    /** kill -<sig> -<pid> (negative pid = whole GROUP). Returns the exit code. */
    protected int signalGroup(String pid, String sig) {
        return run(new String[]{"/bin/sh", "-c", "kill -" + sig + " -" + pid});
    }

    /** True iff any member of group <pid> is still alive (kill -0 -pid == 0). */
    protected boolean groupAlive(String pid) {
        return signalGroup(pid, "0") == 0;
    }

    /**
     * Run cmd, wait up to EXEC_CAP_MS, then destroy it. Java 1.4 has no
     * Process.waitFor(timeout), so wait in a daemon thread and join with a
     * timeout -- this is what keeps stop() from ever hanging the framework
     * on a wedged helper.
     */
    private static int run(final String[] cmd) {
        try {
            final Process p = Runtime.getRuntime().exec(cmd);
            final int[] rc = { -1 };
            Thread t = new Thread(new Runnable() {
                public void run() { try { rc[0] = p.waitFor(); } catch (InterruptedException ie) { } }
            });
            t.setDaemon(true); t.start();
            t.join(EXEC_CAP_MS);
            if (t.isAlive()) { p.destroy(); return -1; }
            return rc[0];
        } catch (Throwable x) { return -1; }
    }

    private static String firstLine(final String[] cmd) {
        try {
            final Process p = Runtime.getRuntime().exec(cmd);
            final String[] out = { "" };
            Thread t = new Thread(new Runnable() {
                public void run() {
                    try {
                        BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), "US-ASCII"));
                        String line = r.readLine();
                        r.close();
                        try { p.waitFor(); } catch (InterruptedException ie) { /* ignore */ }
                        out[0] = line == null ? "" : line.trim();
                    } catch (Throwable t2) { }
                }
            });
            t.setDaemon(true); t.start();
            t.join(EXEC_CAP_MS);
            if (t.isAlive()) { p.destroy(); return ""; }
            return out[0];
        } catch (Throwable x) { return ""; }
    }
}
