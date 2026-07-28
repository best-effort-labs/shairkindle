// kindlet/test/com/besteffortlabs/kindletshell/LeaseConnectionTest.java
package com.besteffortlabs.kindletshell;

import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;

public class LeaseConnectionTest {

    // 6-byte test frame; recovers kind + seq so the reader can check order/gaps.
    static final Encoder ENC = new Encoder() {
        public byte[] encode(OutboundEvent e, int seq) {
            return new byte[] { (byte) e.kind, (byte) (e.pressed ? 1 : 0),
                (byte) (e.keyCode >>> 8), (byte) e.keyCode, (byte) (seq >>> 8), (byte) seq };
        }
    };

    static class Pair { int kind, seq; Pair(int k, int s){kind=k;seq=s;} }

    // Read n 6-byte frames (blocking) and decode kind+seq.
    static ArrayList readFrames(InputStream in, int n) throws Exception {
        ArrayList out = new ArrayList();
        byte[] f = new byte[6];
        for (int i = 0; i < n; i++) {
            int off = 0; while (off < 6) { int r = in.read(f, off, 6 - off); if (r < 0) throw new Exception("EOF"); off += r; }
            out.add(new Pair(f[0] & 0xff, ((f[4] & 0xff) << 8) | (f[5] & 0xff)));
        }
        return out;
    }

    public static void main(String[] args) throws Exception {
        testHelloFirstAndSeqOrder();
        testDropConsumesSeqGap();
        testIdleHeartbeat();
        testMonotonicSeqUnderHeartbeatRace();
        testCloseIdempotent();
        TestSupport.done("LeaseConnectionTest");
    }

    // HELLO is seq 0 and arrives before any key; keys are seq 1,2,3 in order.
    static void testHelloFirstAndSeqOrder() throws Exception {
        ServerSocket srv = new ServerSocket(0);
        Socket c = new Socket("127.0.0.1", srv.getLocalPort());
        Socket s = srv.accept();
        final boolean[] up = { false };
        LeaseConnection lc = new LeaseConnection(c, ENC, 100000,
            new LeaseConnection.Listener(){ public void onLeaseState(boolean u){ up[0]=u; } });
        lc.start();
        lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, 10, true, 0));
        lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, 11, true, 0));
        lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, 12, true, 0));
        ArrayList f = readFrames(s.getInputStream(), 4);
        TestSupport.check(((Pair)f.get(0)).kind == 0 && ((Pair)f.get(0)).seq == 0, "hello first, seq0");
        TestSupport.check(((Pair)f.get(1)).seq == 1 && ((Pair)f.get(2)).seq == 2 && ((Pair)f.get(3)).seq == 3, "keys seq 1,2,3 in order");
        TestSupport.check(up[0], "lease came up after hello");
        lc.close(); s.close(); srv.close();
    }

    // Overflow the queue before the writer drains: a dropped KEY still consumed a
    // seq, so the surviving frames show a seq gap (not a dense 0..N run).
    static void testDropConsumesSeqGap() throws Exception {
        ServerSocket srv = new ServerSocket(0);
        Socket c = new Socket("127.0.0.1", srv.getLocalPort());
        Socket s = srv.accept();
        // Do NOT read yet, and never let the writer send until the queue is stuffed:
        // fill well past QUEUE_CAP so early events are dropped.
        LeaseConnection lc = new LeaseConnection(c, ENC, 100000,
            new LeaseConnection.Listener(){ public void onLeaseState(boolean u){} });
        for (int i = 0; i < LeaseConnection.QUEUE_CAP + 50; i++)
            lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, i, true, 0));
        lc.start(); // writer now drains: HELLO(seq varies) + survivors
        // Read a batch and confirm the max seq exceeds the number of surviving KEY
        // frames — i.e. seq numbers were consumed by dropped events (a real gap).
        ArrayList f = readFrames(s.getInputStream(), 20);
        int maxSeq = 0; for (int i = 0; i < f.size(); i++) maxSeq = Math.max(maxSeq, ((Pair)f.get(i)).seq);
        TestSupport.check(maxSeq >= LeaseConnection.QUEUE_CAP, "dropped events consumed seqs -> gap (maxSeq " + maxSeq + ")");
        lc.close(); s.close(); srv.close();
    }

    // With no traffic, a heartbeat frame (kind 2) appears after ~heartbeatMs.
    static void testIdleHeartbeat() throws Exception {
        ServerSocket srv = new ServerSocket(0);
        Socket c = new Socket("127.0.0.1", srv.getLocalPort());
        Socket s = srv.accept();
        LeaseConnection lc = new LeaseConnection(c, ENC, 150,
            new LeaseConnection.Listener(){ public void onLeaseState(boolean u){} });
        lc.start();
        ArrayList f = readFrames(s.getInputStream(), 2); // HELLO then HEARTBEAT
        TestSupport.check(((Pair)f.get(0)).kind == 0, "hello");
        TestSupport.check(((Pair)f.get(1)).kind == 2, "heartbeat after idle");
        lc.close(); s.close(); srv.close();
    }

    // Enqueue keys while heartbeats fire in the same window, and assert seq is
    // STRICTLY increasing on the wire. The old code minted the heartbeat's seq in
    // a separate critical section from the empty-queue check, so a KEY enqueued in
    // the gap got seq N while the heartbeat got N+1 and was written first -> a seq
    // reversal (N+1 then N). The fix mints the heartbeat atomically with the check.
    static void testMonotonicSeqUnderHeartbeatRace() throws Exception {
        ServerSocket srv = new ServerSocket(0);
        Socket c = new Socket("127.0.0.1", srv.getLocalPort());
        Socket s = srv.accept();
        final LeaseConnection lc = new LeaseConnection(c, ENC, 2,
            new LeaseConnection.Listener(){ public void onLeaseState(boolean u){} });
        lc.start();
        final int N = 120;
        Thread prod = new Thread(new Runnable() {
            public void run() {
                for (int i = 0; i < N; i++) {
                    lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, i, true, 0));
                    try { Thread.sleep(2); } catch (InterruptedException ie) {}  // let heartbeats interleave
                }
            }
        });
        prod.start();
        // Read frames until we see the last key (keyCode N-1); assert monotonic seq.
        InputStream in = s.getInputStream();
        byte[] f = new byte[6];
        int prevSeq = -1, heartbeats = 0;
        boolean sawLast = false;
        for (int guard = 0; guard < N * 4 && !sawLast; guard++) {
            int off = 0; while (off < 6) { int r = in.read(f, off, 6 - off); if (r < 0) throw new Exception("EOF"); off += r; }
            int kind = f[0] & 0xff;
            int seq  = ((f[4] & 0xff) << 8) | (f[5] & 0xff);
            int code = ((f[2] & 0xff) << 8) | (f[3] & 0xff);
            TestSupport.check(seq > prevSeq, "seq strictly increasing on wire (" + prevSeq + " -> " + seq + ")");
            prevSeq = seq;
            if (kind == 2) heartbeats++;
            if (kind == OutboundEvent.KIND_KEY && code == N - 1) sawLast = true;
        }
        prod.join();
        TestSupport.check(sawLast, "read through the last key");
        TestSupport.check(heartbeats > 0, "heartbeats interleaved with keys (window exercised)");
        lc.close(); s.close(); srv.close();
    }

    // close() is idempotent, never throws, and enqueue-after-close returns false.
    static void testCloseIdempotent() throws Exception {
        ServerSocket srv = new ServerSocket(0);
        Socket c = new Socket("127.0.0.1", srv.getLocalPort());
        Socket s = srv.accept();
        LeaseConnection lc = new LeaseConnection(c, ENC, 100000,
            new LeaseConnection.Listener(){ public void onLeaseState(boolean u){} });
        lc.start();
        lc.close(); lc.close(); // twice, no throw
        TestSupport.check(!lc.enqueue(new OutboundEvent(OutboundEvent.KIND_KEY, 1, true, 0)), "enqueue after close = false");
        s.close(); srv.close();
    }
}
