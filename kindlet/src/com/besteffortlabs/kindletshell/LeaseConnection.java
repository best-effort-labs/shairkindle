package com.besteffortlabs.kindletshell;

import java.io.IOException;
import java.io.OutputStream;
import java.net.Socket;
import java.util.LinkedList;

/**
 * One synchronized queue funnels all producers;
 * seq is assigned at ENQUEUE under the lock (so a dropped KEY still burns its seq
 * and the receiver sees a real gap). A single writer thread drains strict FIFO,
 * encodes and writes OUTSIDE the lock, synthesizes idle heartbeats, and owns the
 * HELLO handshake. close() is idempotent, deadlock-free, and never self-joins.
 */
public final class LeaseConnection {

    public static final int QUEUE_CAP = 256;

    public interface Listener { void onLeaseState(boolean up); }

    private static final class Framed {
        final int seq; final OutboundEvent e;
        Framed(int seq, OutboundEvent e) { this.seq = seq; this.e = e; }
    }

    private final Object lock = new Object();
    private final LinkedList queue = new LinkedList();   // of Framed, raw (1.4)
    private final Encoder enc;
    private final int heartbeatMs;
    private final Listener listener;

    private Socket sock;                 // nulled on close, under lock
    private OutputStream out;
    private int nextSeq = 0;             // guarded by lock
    private int drops = 0;               // guarded by lock
    private boolean closed = false;      // guarded by lock
    private boolean up = false;          // lease-up latch, guarded by lock
    private boolean down = false;        // lease-down fired latch, guarded by lock

    private Thread writer;
    private int helloSeq;                 // pinned in start(), guarded by lock

    public LeaseConnection(Socket sock, Encoder enc, int heartbeatMs, Listener l) {
        this.sock = sock;
        this.enc = enc;
        this.heartbeatMs = heartbeatMs;
        this.listener = l;
    }

    /**
     * Pin HELLO's seq in the CALLING thread (under the lock) before spawning the
     * writer, then spawn it. Pinning here — not inside the writer — makes the
     * seq deterministic against a producer that enqueues immediately after
     * start() (the writer thread may not have run yet): HELLO reserves the next
     * seq, so subsequent keys get the seqs that follow it, and the writer sends
     * the pre-built HELLO frame first.
     */
    public void start() {
        synchronized (lock) {
            if (closed) return;
            helloSeq = nextSeq++;
        }
        writer = new Thread(new Runnable() { public void run() { writerLoop(); } }, "lease-writer");
        writer.setDaemon(true);
        writer.start();
    }

    /**
     * Assign seq under the lock and append. Drop-oldest ONLY for KEY events when
     * full (a dropped KEY has already consumed its seq -> gap). Returns false if
     * closed.
     *
     * CONTRACT: start() must be called before enqueue(). Events enqueued before
     * start() are still sent in wire order but carry sequence numbers BELOW
     * HELLO's — the KindletShell wiring always calls start() before exposing the
     * connection to the router, so keys never precede HELLO in practice.
     */
    public boolean enqueue(OutboundEvent e) {
        synchronized (lock) {
            if (closed) return false;
            if (queue.size() >= QUEUE_CAP && e.kind == OutboundEvent.KIND_KEY) {
                queue.removeFirst();
                drops++;
            }
            int seq = nextSeq++;
            queue.addLast(new Framed(seq, e));
            lock.notifyAll();
            return true;
        }
    }

    /**
     * close() — idempotent, deadlock-free. Under the lock: set closed, detach the
     * socket, notify. Close the socket OUTSIDE the lock. Fire onLeaseState(false)
     * once (only if it had come up), never while holding a lock, never self-join.
     */
    public void close() {
        Socket toClose;
        synchronized (lock) {
            if (closed) return;
            closed = true;
            toClose = sock;
            sock = null;
            out = null;
            lock.notifyAll();
        }
        if (toClose != null) { try { toClose.close(); } catch (Throwable t) { /* best effort */ } }
        markDown();
    }

    // --- writer thread ---------------------------------------------------

    private void writerLoop() {
        // Grab the output stream under the lock. NOTE: seq is monotonic from
        // construction — we deliberately do NOT reset nextSeq or clear the queue
        // here (see the seq-epoch note in the class doc / task-3-report.md). A
        // LeaseConnection is one-shot per Socket, so there is no stale prior-lease
        // input to flush, and the drop-gap test relies on events enqueued before
        // start() surviving with their assigned seqs. On the normal path (nothing
        // enqueued pre-start) HELLO is seq 0 and the first KEY is seq 1.
        int hs;
        synchronized (lock) {
            if (closed) return;
            hs = helloSeq;
            try { out = sock.getOutputStream(); } catch (Throwable t) { out = null; }
        }
        if (!writeOne(new Framed(hs, helloEvent()))) { close(); return; }
        markUp();

        long lastWrite = now();
        while (true) {
            Framed fr;
            synchronized (lock) {
                while (!closed && queue.isEmpty()) {
                    long w = heartbeatMs - (now() - lastWrite);
                    if (w <= 0) break;
                    try { lock.wait(w); } catch (InterruptedException ie) { /* re-check */ }
                }
                if (closed) break;
                if (!queue.isEmpty()) {
                    fr = (Framed) queue.removeFirst();
                } else {
                    // idle timeout with an empty queue -> heartbeat, seq minted
                    // atomically here so the "queue empty" decision and the seq
                    // assignment can't be split by a producer enqueuing a KEY in
                    // between (that window let the heartbeat overtake the key seq).
                    fr = new Framed(nextSeq++,
                        new OutboundEvent(OutboundEvent.KIND_HEARTBEAT, 0, false, (int) now()));
                }
            }
            if (!writeOne(fr)) break;   // write error -> close self (below)
            lastWrite = now();
        }
        close();
    }

    private OutboundEvent helloEvent() {
        return new OutboundEvent(OutboundEvent.KIND_HELLO, 0, false, (int) now());
    }

    /**
     * Encode (outside the lock) and write+flush. An encoder throw is logged and the
     * frame skipped (returns true — a bad frame doesn't kill the lease). An
     * IOException on the socket returns false (kills the lease).
     */
    private boolean writeOne(Framed fr) {
        byte[] b;
        try {
            b = enc.encode(fr.e, fr.seq);
        } catch (Throwable t) {
            return true;   // skip this frame, keep the lease alive
        }
        if (b == null || b.length == 0) return true;
        OutputStream o;
        synchronized (lock) { o = out; if (closed) return false; }
        if (o == null) return false;
        try {
            o.write(b);
            o.flush();
            return true;
        } catch (IOException ioe) {
            return false;
        }
    }

    private void markUp() {
        boolean fire = false;
        synchronized (lock) { if (!closed && !up) { up = true; fire = true; } }
        if (fire && listener != null) listener.onLeaseState(true);
    }

    private void markDown() {
        boolean fire = false;
        synchronized (lock) { if (up && !down) { down = true; fire = true; } }
        if (fire && listener != null) listener.onLeaseState(false);
    }

    private static long now() { return System.currentTimeMillis(); }
}
