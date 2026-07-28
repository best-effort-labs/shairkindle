package com.besteffortlabs.shairkindle;

import com.besteffortlabs.kindletshell.TestSupport;
import java.util.ArrayList;

/** Unit-test teardownAndWait's kill sequencing against a mock kill/poll. */
public class TeardownSequenceTest {

    static class MockApp extends ShairKindle {
        final ArrayList calls = new ArrayList();   // "TERM","POLL","KILL"
        String pid = "4000";
        int aliveForPolls = 2;                     // group dies after N polls
        protected String resolveSupervisorPid() { return pid; }
        protected int signalGroup(String p, String sig) { calls.add(sig + ":" + p); return 0; }
        protected boolean groupAlive(String p) {
            calls.add("POLL:" + p);
            return aliveForPolls-- > 0;
        }
        protected long sleepStep() { return 0; }   // no real sleeping in the unit test
    }

    public static void main(String[] a) {
        // group dies after 2 polls -> TERM, poll*, no KILL
        MockApp m = new MockApp();
        m.teardownAndWait(800);
        TestSupport.check(m.calls.contains("TERM:4000"), "group TERM sent");
        TestSupport.check(!m.calls.contains("KILL:4000"), "no KILL when group dies in time");

        // group never dies within the deadline -> TERM then KILL
        MockApp hung = new MockApp();
        hung.aliveForPolls = Integer.MAX_VALUE;
        hung.teardownAndWait(150);
        TestSupport.check(hung.calls.contains("TERM:4000"), "hung: TERM sent");
        TestSupport.check(hung.calls.contains("KILL:4000"), "hung: KILL after deadline");

        // no supervisor -> resolver returns empty -> nothing signalled
        MockApp none = new MockApp();
        none.pid = "";
        none.teardownAndWait(800);
        TestSupport.check(none.calls.isEmpty(), "no pid -> no signals");

        TestSupport.done("TeardownSequenceTest");
    }
}
