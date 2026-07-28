package com.besteffortlabs.kindletshell;

import com.amazon.kindle.kindlet.KindletContext;
import java.awt.Component;
import java.awt.Container;
import java.io.File;

/** Host tests for the stop()/launch interlock -- the load-bearing fix. */
public class KindletShellInterlockTest {

    static KindletContext ctx() {
        return new KindletContext() {
            private final Container root = new Container();
            public Container getRootContainer() { return root; }
            public File getHomeDirectory() { return new File("."); }
            public void setSubTitle(String s) { }
        };
    }

    /** A shell whose native phases are injectable; records ordering. */
    static class Probe extends KindletShell {
        volatile long launchStartedAt = -1, launchReturnedAt = -1, teardownAt = -1, releaseAt = -1;
        volatile int launchCount = 0;
        volatile int releaseCount = 0;
        volatile boolean ready = false;
        long injectLaunchMs = 0;
        long deadline = 800;                       // default; testStopBounded... overrides to 200
        final Object permissionGate = new Object();
        boolean gatePermissions = false;

        protected PayloadSpec payload() {
            return new PayloadSpec("/tmp", 59999, 10000, "x", new String[0], "x.log", "x-super", "/tmp/none.pid");
        }
        protected Encoder encoder() { return new Encoder() { public byte[] encode(OutboundEvent e, int s){ return new byte[0]; } }; }
        protected Component display() { return new Component(){ public void paint(java.awt.Graphics g){} }; }

        protected boolean acquireRuntimePermissions() {
            if (gatePermissions) {
                synchronized (permissionGate) {
                    try { permissionGate.wait(2000); } catch (InterruptedException ie){}
                }
            }
            return true;
        }
        protected void releaseRuntimePermissions() {
            releaseCount++;
            releaseAt = System.currentTimeMillis();
        }
        protected void installPayload() { /* no-op */ }
        protected void launchNative() {
            launchCount++;
            launchStartedAt = System.currentTimeMillis();
            try { Thread.sleep(injectLaunchMs); } catch (InterruptedException ie) {}
            ready = true;                          // pidfile "appears" after the launch body
            launchReturnedAt = System.currentTimeMillis();
        }
        protected boolean nativeReady() { return ready; }
        protected long teardownDeadlineMs() { return deadline; }
        protected void teardownAndWait(long d) { teardownAt = System.currentTimeMillis(); }
    }

    public static void main(String[] a) throws Exception {
        testStopAwaitsInFlightLaunch();
        testNoLaunchAfterStop();
        testStopBoundedByDeadlineWhenLaunchHangs();
        testLaunchFailsFastWhenChildNotAlive();
        testAliveWithoutPidfileRetriesThenSurfaces();
        testTeardownThrowInvokesFallback();
        testDestroyReleasesPrivileges();
        TestSupport.done("KindletShellInterlockTest");
    }

    // stop() during launchNative must BLOCK until the launch section finishes
    // (pidfile observed), THEN run teardownAndWait -- never tear down first.
    static void testStopAwaitsInFlightLaunch() throws Exception {
        Probe p = new Probe();
        p.injectLaunchMs = 300;
        p.create(ctx());
        p.start();
        Thread.sleep(80);                          // let the worker reach launchNative()
        TestSupport.check(p.launchStartedAt > 0 && p.launchReturnedAt < 0, "launch is in flight");
        p.stop();                                  // must await the launch section
        TestSupport.check(p.launchReturnedAt > 0, "launch completed before stop returned");
        TestSupport.check(p.teardownAt >= p.launchReturnedAt, "teardown ran AFTER launch finished");
    }

    // A stop() that lands before the launch phase must prevent the launch entirely.
    static void testNoLaunchAfterStop() throws Exception {
        Probe p = new Probe();
        p.gatePermissions = true;                  // worker blocks in permission acquisition
        p.create(ctx());
        p.start();
        Thread.sleep(80);                          // worker now parked in permission acquisition
        p.stop();                                  // sets stopped + bumps generation
        synchronized (p.permissionGate) { p.permissionGate.notifyAll(); }
        Thread.sleep(200);                         // worker proceeds, must bail before launch
        TestSupport.check(p.launchCount == 0, "no native launch after stop (got " + p.launchCount + ")");
    }

    // A launch whose supervisor never registers AND is not running must fail FAST:
    // one attempt (not LAUNCH_ATTEMPTS retries), and the failure is surfaced to the UI
    // (setSubTitle) rather than leaving a silent blank screen. Regression for the on-device
    // "stale supervisor squats the port -> guard skips launch -> blank screen" wedge.
    static void testLaunchFailsFastWhenChildNotAlive() throws Exception {
        final int[] errCount = {0};
        KindletContext spy = new KindletContext() {
            private final Container root = new Container();
            public Container getRootContainer() { return root; }
            public File getHomeDirectory() { return new File("."); }
            public void setSubTitle(String s) { errCount[0]++; }
        };
        Probe p = new Probe() {
            protected void launchNative() { launchCount++; }   // never registers a pidfile
            protected boolean nativeReady() { return false; }
            protected boolean nativeAlive() { return false; }  // the fork already exited / never came up
        };
        p.create(spy);
        p.start();
        Thread.sleep(2000);                        // > 3 full poll windows: buggy code would retry to launchCount==3
        TestSupport.check(p.launchCount == 1, "one launch attempt when child not alive (got " + p.launchCount + ")");
        TestSupport.check(errCount[0] >= 1, "launch failure surfaced via setSubTitle (got " + errCount[0] + ")");
    }

    // The stale-supervisor case (on-device root cause): a process matches the ps token
    // (nativeAlive()==true) but no fresh pidfile ever appears -- the fast-fail break must
    // NOT fire (it would mistake the squatter for our fork). All LAUNCH_ATTEMPTS run, then
    // the failure is surfaced. Complements testLaunchFailsFastWhenChildNotAlive.
    static void testAliveWithoutPidfileRetriesThenSurfaces() throws Exception {
        final int[] errCount = {0};
        KindletContext spy = new KindletContext() {
            private final Container root = new Container();
            public Container getRootContainer() { return root; }
            public File getHomeDirectory() { return new File("."); }
            public void setSubTitle(String s) { errCount[0]++; }
        };
        Probe p = new Probe() {
            protected void launchNative() { launchCount++; }   // never registers a pidfile
            protected boolean nativeReady() { return false; }
            protected boolean nativeAlive() { return true; }   // a squatter matches the token
        };
        p.create(spy);
        p.start();
        Thread.sleep(3000);                        // 3 attempts x 500ms poll + overhead
        TestSupport.check(p.launchCount == 3, "all attempts run when a process is alive (got " + p.launchCount + ")");
        TestSupport.check(errCount[0] >= 1, "launch failure surfaced after retries (got " + errCount[0] + ")");
    }

    // A subclass teardownAndWait that THROWS must not silently lose the teardown: stop()
    // catches it and invokes teardownFallback() so the native side still gets killed
    // synchronously, rather than falling back to only the lease-EOF self-exit.
    static void testTeardownThrowInvokesFallback() throws Exception {
        final boolean[] fallbackRan = {false};
        Probe p = new Probe() {
            protected void teardownAndWait(long d) { throw new RuntimeException("boom"); }
            protected void teardownFallback() { fallbackRan[0] = true; }
        };
        p.create(ctx());
        p.start();
        Thread.sleep(80);                          // let the worker settle past launch
        p.stop();                                  // teardownAndWait throws -> fallback must run
        TestSupport.check(fallbackRan[0], "teardownFallback invoked when teardownAndWait throws");
    }

    // destroy() must restore runtime permission state even when stop() is
    // otherwise a no-op/already completed. Cleanup belongs to destroy, matching
    // ixtab's SuicidalKindlet lifecycle rather than every temporary stop/start.
    static void testDestroyReleasesPrivileges() {
        Probe p = new Probe();
        p.create(ctx());
        p.destroy();
        TestSupport.check(p.releaseCount == 1,
                "destroy releases runtime privileges exactly once");
        TestSupport.check(p.teardownAt > 0 && p.releaseAt >= p.teardownAt,
                "destroy releases runtime privileges only after native teardown");
    }

    // If launchNative hangs past the deadline, stop() must still return within ~deadline.
    static void testStopBoundedByDeadlineWhenLaunchHangs() throws Exception {
        Probe p = new Probe();
        p.injectLaunchMs = 5000;                   // never becomes ready in time
        p.deadline = 200;
        p.create(ctx());
        p.start();
        Thread.sleep(80);
        long t0 = System.currentTimeMillis();
        p.stop();
        long dt = System.currentTimeMillis() - t0;
        TestSupport.check(dt < 1200, "stop() bounded by deadline (took " + dt + "ms)");
        TestSupport.check(p.teardownAt > 0, "teardownAndWait still invoked past the await deadline");
    }
}
