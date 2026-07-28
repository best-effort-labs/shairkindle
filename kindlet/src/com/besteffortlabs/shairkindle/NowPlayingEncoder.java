package com.besteffortlabs.shairkindle;
import com.besteffortlabs.kindletshell.Encoder;
import com.besteffortlabs.kindletshell.OutboundEvent;

// App-A wire codec (spec Appendix). Stateful leading-edge gate: one transport
// command per physical press. Called only on LeaseConnection's writer thread,
// so pressActive needs no synchronization.
public final class NowPlayingEncoder implements Encoder {
    private boolean pressActive;
    public byte[] encode(OutboundEvent e, int seq) {
        if (e.kind == OutboundEvent.KIND_HELLO)     return ascii("HELLO 1\n");
        if (e.kind == OutboundEvent.KIND_HEARTBEAT) return ascii("PING\n");
        // KEY
        if (!e.pressed) { pressActive = false; return null; }
        if (pressActive) return null;               // held-bar auto-repeat
        String cmd = map(e.keyCode);
        if (cmd == null) return null;               // unmapped: don't arm
        pressActive = true;
        return ascii(cmd);
    }
    private static String map(int code) {
        switch (code) {
            case 61448: case 61449: return "NEXT\n";
            case 61450: return "PREV\n";
            case 61451: return "PLAYPAUSE\n";
            default: return null;
        }
    }
    private static byte[] ascii(String s) {
        try { return s.getBytes("US-ASCII"); } catch (Exception ex) { return s.getBytes(); }
    }
}
