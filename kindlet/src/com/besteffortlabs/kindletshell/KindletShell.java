package com.besteffortlabs.kindletshell;

import com.amazon.kindle.kindlet.AbstractKindlet;
import com.amazon.kindle.kindlet.KindletContext;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.Container;
import java.awt.EventQueue;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * Abstract lifecycle coordinator for the Kindlet. Framework-bound (extends
 * {@link AbstractKindlet}), compile-checked here and validated on device.
 *
 * <p>The subtle part is the lifecycle-lock / generation / connect-publish protocol.
 * A single {@link #lifecycleLock} guards {@link #generation}, {@link #stopped},
 * {@link #conn} and {@link #worker}. Each start() bumps the generation and captures it
 * as {@code G}; the worker checks {@link #isCurrent(int)} before and after every phase,
 * so a stop() (which bumps the generation and sets stopped) makes every in-flight worker
 * a no-op and guarantees no stopped shell ever keeps a live lease.
 *
 * <p>Subclasses supply {@link #payload()}, {@link #encoder()} and {@link #display()}, and
 * may override {@link #onLeaseState(boolean)} (default no-op).
 */
public abstract class KindletShell extends AbstractKindlet {

    // --- subclass contract ---------------------------------------------------

    private final RuntimePermissions runtimePermissions = new RuntimePermissions();

    protected abstract PayloadSpec payload();
    protected abstract Encoder encoder();
    protected abstract Component display();

    /** Called on the EDT when the lease transitions up/down. Default: no-op. */
    protected void onLeaseState(boolean up) { }

    /** Tear down all native processes this app started and BLOCK until they are
     *  gone (or deadlineMs elapses). Called from stop() inside the framework's
     *  stop()-blocking window. Best-effort past the deadline. Default: no-op. */
    protected void teardownAndWait(long deadlineMs) { }

    /** Last-ditch, best-effort kill invoked by stop() ONLY when {@link #teardownAndWait}
     *  THROWS -- so a misbehaving subclass override still gets one shot at killing what it
     *  started, preserving the synchronous-teardown guarantee. A throwing teardown
     *  is otherwise backstopped by the native side's own lease-EOF self-exit (stop() closes
     *  the lease before calling teardownAndWait), which is correct but async (~seconds);
     *  this hook keeps teardown synchronous on the throw path. Default no-op (rely on the
     *  backstop). MUST itself never throw. */
    protected void teardownFallback() { }

    // --- overridable native phases (defaults = production behaviour) ---
    protected boolean acquireRuntimePermissions() { return runtimePermissions.acquire(); }
    /** Restore runtime policy state during destroy. Separate seam keeps lifecycle testable. */
    protected void releaseRuntimePermissions() { runtimePermissions.release(); }
    protected void launchNative() { new NativeLauncher().launch(payload()); }
    /** Readiness = the native process published its pidfile (registered + killable).
     *  Existence check only; authoritative PID/SID/starttime validation is
     *  teardown-side, where killing the wrong group as root is the real hazard. */
    protected boolean nativeReady() { return new File(payload().pidfilePath()).exists(); }
    /** Is a native instance actually running? The supervisor is launched DETACHED
     *  (setsid + backgrounded), so the kindlet's real child is a short-lived shell we
     *  cannot waitFor() -- to tell "starting up" from "already exited" we poll for its
     *  process. Used to fail a launch fast once the fork is gone. Default:
     *  the same ps-guard the launcher uses.
     *
     *  <p>Bounded (PS_PROBE_CAP_MS): a wedged ps/sh must NOT hang the worker with
     *  {@code launching} still set -- runWorker's prev.join() is unbounded, so a hung
     *  probe would deadlock the NEXT start(). On timeout OR error we assume alive (return
     *  true): never false-fail a launch that might still be coming up. A false NEGATIVE
     *  here is backstopped by the supervisor's pre-lease accept() timeout, which reaps any
     *  orphan we wrongly gave up on. */
    protected boolean nativeAlive() {
        int rc = execCapped(
                new String[]{"/bin/sh", "-c", NativeLauncher.psGuard(payload().psMatchToken())},
                PS_PROBE_CAP_MS);
        if (rc == 0) return true;    // grep matched -> a supervisor is running
        if (rc > 0) return false;    // grep found nothing -> the fork is gone
        return true;                 // rc < 0: probe timed out / failed -> assume alive, don't false-fail
    }
    private static final long PS_PROBE_CAP_MS = 500L;

    /** exec cmd, wait up to capMs, then destroy. Java 1.4 has no Process.waitFor(timeout),
     *  so wait in a daemon thread and join with a timeout. Returns the exit code, or -1 on
     *  exec failure OR timeout (caller distinguishes as needed). */
    private static int execCapped(final String[] cmd, long capMs) {
        try {
            final Process p = Runtime.getRuntime().exec(cmd);
            final int[] rc = { -1 };
            Thread t = new Thread(new Runnable() {
                public void run() { try { rc[0] = p.waitFor(); } catch (InterruptedException ie) { } }
            });
            t.setDaemon(true); t.start();
            t.join(capMs);
            if (t.isAlive()) { p.destroy(); return -1; }
            return rc[0];
        } catch (Throwable x) { return -1; }
    }
    /** Install/update the payload. Override in tests. Throws on failure. */
    protected void installPayload() throws Exception {
        InputStream min = getClass().getResourceAsStream("/payload/manifest");
        if (min == null) throw new java.io.IOException("no bundled /payload/manifest");
        String text = readToString(min);
        PayloadInstaller.Manifest m = PayloadInstaller.parseManifest(text);
        PayloadInstaller installer = new PayloadInstaller();
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String relPath) {
                return getClass().getResourceAsStream("/payload/" + relPath);
            }
        };
        File dir = new File(payload().installDir());
        if (!installer.isUpToDate(dir, m.version)) installer.install(dir, m, res);
    }

    // --- lifecycle state -----------------------------------------------------

    private final Object lifecycleLock = new Object();
    private int generation = 0;                 // guarded by lifecycleLock
    private boolean stopped = false;            // guarded by lifecycleLock
    private LeaseConnection conn;               // guarded by lifecycleLock
    private Thread worker;                      // guarded by lifecycleLock
    private boolean launching = false;          // guarded by lifecycleLock
    /** Hard ceiling for stop(): launch-await + kill/poll combined. */
    protected long teardownDeadlineMs() { return 800L; }
    private static final int LAUNCH_ATTEMPTS = 3;   // relaunch until pidfile appears
    // Per-attempt pidfile-observe bound. Must exceed a healthy supervisor's
    // setsid+bind+write time (~ms) yet stay under teardownDeadlineMs() so stop()'s
    // await comfortably covers the common single-attempt case.
    private static final int READY_POLLS = 5;       // per attempt (x100ms = 500ms)
    private static final int READY_POLL_MS = 100;

    private KindletContext ctx;
    private Component displayComp;
    private InputRouter router;

    /** Only ever read/written on the EDT: -1 unknown, 0 down, 1 up. */
    private int lastUp = -1;

    protected KindletContext getContext() { return ctx; }

    // --- framework lifecycle -------------------------------------------------

    public void create(KindletContext context) {
        this.ctx = context;
        this.displayComp = display();
        this.router = new InputRouter();
        Container root = context.getRootContainer();
        root.setLayout(new BorderLayout());
        root.add(displayComp, BorderLayout.CENTER);
        // Keys reach a component only when it has focus, and AWT won't give focus
        // to a zero-size / not-yet-showing component -- so route through the
        // real-size CENTER component (validated on-device). An earlier 0x0-SOUTH router never took focus,
        // so no key ever arrived. router is used purely as the KeyListener here.
        displayComp.setFocusable(true);
        displayComp.addKeyListener(router);
        displayComp.requestFocus();
    }

    public void start() {
        displayComp.requestFocus();     // re-assert focus on (re)start; keys route via displayComp's KeyListener
        LeaseConnection old;
        synchronized (lifecycleLock) {
            generation++;
            stopped = false;
            old = conn;             // detach a prior generation's lease (re-start without stop)
            conn = null;
            final int g = generation;
            final Thread prev = worker;
            worker = new Thread(new Runnable() {
                public void run() { runWorker(g, prev); }
            });
            worker.setDaemon(true);
            worker.start();
        }
        if (old != null) { router.setLease(null); old.close(); }   // close OUTSIDE the lock
    }

    public void stop() {
        LeaseConnection c;
        long start = System.currentTimeMillis();
        long deadline = teardownDeadlineMs();
        synchronized (lifecycleLock) {
            generation++;
            stopped = true;
            c = conn;
            conn = null;
            // Await an in-flight native launch: the worker may be mid
            // launchNative() or between launch and pidfile-observed. Tearing down now
            // would miss a supervisor it forks right after. Bounded by the shared deadline.
            while (launching) {
                long remaining = deadline - (System.currentTimeMillis() - start);
                if (remaining <= 0) break;                 // best-effort past the ceiling
                try { lifecycleLock.wait(remaining); } catch (InterruptedException ie) { break; }
            }
        }
        if (router != null) router.setLease(null);
        if (c != null) c.close();   // close OUTSIDE the lock
        long remaining = deadline - (System.currentTimeMillis() - start);
        try { teardownAndWait(remaining > 0 ? remaining : 0); }
        catch (Throwable t) {
            log("teardownAndWait failed: " + t);
            try { teardownFallback(); }
            catch (Throwable t2) { log("teardownFallback failed: " + t2); }  // last resort: native lease-EOF self-exit
        }
    }

    public void destroy() {
        try {
            stop();   // idempotent
        } finally {
            /* Match ixtab's SuicidalKindlet lifecycle: remove the in-memory
             * policy wrapper only if our bootstrap was the code that enabled it. */
            releaseRuntimePermissions();
        }
    }

    // --- worker --------------------------------------------------------------

    private void runWorker(int g, Thread prev) {
        try {
            if (prev != null) { try { prev.join(); } catch (InterruptedException ie) { /* ignore */ } }
            if (!isCurrent(g)) return;
            if (!acquireRuntimePermissions()) { fail("permissions"); return; }
            if (!isCurrent(g)) return;
            if (!doInstall(g)) return;
            if (!isCurrent(g)) return;
            // launch failed (no supervisor registered). Surface an error rather than a
            // silent blank screen -- but only if still current: a stop()-driven false is
            // a normal shutdown, not a failure.
            if (!launchAndAwaitReady(g)) { if (isCurrent(g)) fail("launch"); return; }
            connectLoop(g);
        } catch (Throwable t) {
            log("worker crashed: " + t);
        }
    }

    /**
     * Launch the native payload as a guarded critical section: set the {@code launching}
     * latch (under the lock) so a concurrent stop() awaits us instead of tearing down a
     * supervisor we are about to fork; (re)launch until the readiness signal (pidfile)
     * appears; clear the latch (and notify stop()) in a finally. The ps|grep guard in
     * the launch line is NOT the arbiter -- the pidfile is.
     *
     * <p>Invariant: once {@link #launchNative()}
     * has forked, we MUST finish observing the pidfile for THAT fork before clearing
     * {@code launching} -- we do NOT bail early on a generation change inside the observe
     * loop. Bailing early would let a racing stop() tear down BEFORE the supervisor is
     * discoverable, and the fork would survive. Only the decision to start a *new*
     * (re)launch is generation-gated (at the top of the attempt loop).
     */
    private boolean launchAndAwaitReady(int g) {
        synchronized (lifecycleLock) {
            if (generation != g || stopped) return false;   // stop() won the race: never launch
            launching = true;
        }
        try {
            // A forced SIGKILL can leave a stale pidfile; remove it before launch.
            // Delete it so nativeReady()'s existence check authoritatively means "the NEW
            // supervisor published its pidfile", not a leftover from a dead one.
            new File(payload().pidfilePath()).delete();
            for (int attempt = 0; attempt < LAUNCH_ATTEMPTS; attempt++) {
                // Gate only the START of a (re)launch on generation: if stop() already ran,
                // don't fork another supervisor.
                synchronized (lifecycleLock) { if (generation != g || stopped) return false; }
                launchNative();
                // Observe THIS fork's pidfile to completion -- NO generation check here.
                for (int i = 0; i < READY_POLLS; i++) {
                    if (nativeReady()) return true;          // registered + killable
                    sleep(READY_POLL_MS);
                }
                // per-attempt bound elapsed with no pidfile. A healthy supervisor registers
                // within ms of bind(); if none is even running, the fork already exited (bind
                // failed / crashed / the ps-guard skipped launch because a STALE one squats
                // the port). Retrying just forks more doomed children -- give up now so the
                // worker surfaces the failure instead of leaving a blank screen.
                // Safety: this is a name-based snapshot, not proof THIS fork is dead. A false
                // negative (alive supervisor not yet in ps) could clear `launching` early; it
                // is backstopped by the supervisor's pre-lease accept() timeout, which reaps
                // any orphan a racing stop() then misses -- the same net stop()'s own bounded
                // await already relies on.
                if (!nativeAlive()) break;
            }
            return nativeReady();
        } finally {
            synchronized (lifecycleLock) { launching = false; lifecycleLock.notifyAll(); }
        }
    }

    /** Returns false if the install failed (already surfaced) — caller should stop. */
    private boolean doInstall(int g) {
        try { installPayload(); return true; }
        catch (Throwable t) { log("install failed: " + t); fail("install"); return false; }
    }

    private void connectLoop(int g) {
        for (int i = 0; i < 15; i++) {
            if (!isCurrent(g)) return;
            Socket sock = new Socket();
            if (!isCurrent(g)) { closeQuietly(sock); return; }
            try {
                sock.connect(new InetSocketAddress("127.0.0.1", payload().port()), 1000);
            } catch (Throwable t) {
                closeQuietly(sock);
                sleep(200);
                continue;
            }
            publish(g, sock);
            return;
        }
        // connect exhausted -> lease down (leave the focus panel up)
        edtDown(g);
    }

    /**
     * Publish a freshly-connected lease. Ordering invariant: {@code cand.start()}
     * MUST run before {@code router.setLease(cand)} so HELLO reserves seq 0 — a key
     * routed before start() would steal it. Both run inside the lock; if the shell was
     * stopped mid-connect we hand the candidate off to be closed outside the lock.
     */
    private void publish(int g, Socket sock) {
        LeaseConnection cand =
                new LeaseConnection(sock, encoder(), payload().heartbeatMs(), edtListener(g));
        LeaseConnection ext = null;
        synchronized (lifecycleLock) {
            if (!isCurrent(g)) {
                ext = cand;             // stopped mid-connect -> abandon it
            } else {
                conn = cand;
                cand.start();
                router.setLease(cand);
            }
        }
        if (ext != null) ext.close();   // close OUTSIDE the lock
    }

    private boolean isCurrent(int g) {
        synchronized (lifecycleLock) {
            return generation == g && !stopped;
        }
    }

    // --- EDT lease-state marshalling ----------------------------------------

    private LeaseConnection.Listener edtListener(final int g) {
        return new LeaseConnection.Listener() {
            public void onLeaseState(boolean up) { edtApply(g, up); }
        };
    }

    private void edtDown(int g) { edtApply(g, false); }

    /**
     * Marshal a lease-state change to the EDT, generation-guarded (a stale {@code false}
     * behind a new generation's {@code true} is dropped) and de-duped against the last
     * delivered state ({@link #lastUp}, EDT-only, so no lock needed).
     */
    private void edtApply(final int g, final boolean up) {
        EventQueue.invokeLater(new Runnable() {
            public void run() {
                int u = up ? 1 : 0;
                if (isCurrent(g) && u != lastUp) {
                    lastUp = u;
                    onLeaseState(up);
                }
            }
        });
    }

    // --- helpers -------------------------------------------------------------

    private void fail(String phase) {
        log("phase failed: " + phase);
        try { getContext().setSubTitle("error: " + phase); } catch (Throwable t) { /* best effort */ }
    }

    private static void closeQuietly(Socket s) {
        if (s != null) { try { s.close(); } catch (Throwable t) { /* ignore */ } }
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ie) { /* ignore */ }
    }

    private static String readToString(InputStream in) throws java.io.IOException {
        try {
            ByteArrayOutputStream bout = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) >= 0) {
                bout.write(buf, 0, n);
            }
            return new String(bout.toByteArray(), "US-ASCII");
        } finally {
            in.close();
        }
    }

    private static void log(String s) {
        System.out.println("[SHELL] " + s);
        System.out.flush();
    }
}
