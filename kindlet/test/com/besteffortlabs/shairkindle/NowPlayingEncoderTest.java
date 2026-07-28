package com.besteffortlabs.shairkindle;

import com.besteffortlabs.kindletshell.Encoder;
import com.besteffortlabs.kindletshell.OutboundEvent;
import com.besteffortlabs.kindletshell.TestSupport;

public class NowPlayingEncoderTest {
    static String enc(Encoder e, OutboundEvent ev, int seq) throws Exception {
        byte[] b = e.encode(ev, seq); return b == null ? null : new String(b, "US-ASCII");
    }
    static OutboundEvent key(int code, boolean pressed) { return new OutboundEvent(OutboundEvent.KIND_KEY, code, pressed, 0); }

    public static void main(String[] a) throws Exception {
        Encoder e = new NowPlayingEncoder();
        TestSupport.eq(enc(e, new OutboundEvent(OutboundEvent.KIND_HELLO,0,false,0), 0), "HELLO 1\n", "hello");
        TestSupport.eq(enc(e, new OutboundEvent(OutboundEvent.KIND_HEARTBEAT,0,false,0), 5), "PING\n", "heartbeat");
        // leading edge: first press of 5-way select -> PLAYPAUSE; auto-repeat press -> null; release -> null then re-arm
        TestSupport.eq(enc(e, key(61451, true), 1), "PLAYPAUSE\n", "select press");
        TestSupport.eq(enc(e, key(61451, true), 2), null, "held auto-repeat suppressed");
        TestSupport.eq(enc(e, key(61451, false), 3), null, "release sends nothing");
        TestSupport.eq(enc(e, key(61451, true), 4), "PLAYPAUSE\n", "re-armed after release");
        TestSupport.eq(enc(e, key(61451, false), 5), null, "release select before switching keys");
        TestSupport.eq(enc(e, key(61448, true), 6), "NEXT\n", "page-fwd rhs -> NEXT");
        TestSupport.eq(enc(e, key(61448, false), 7), null, "release");
        TestSupport.eq(enc(e, key(61449, true), 8), "NEXT\n", "page-fwd lhs -> NEXT");
        TestSupport.eq(enc(e, key(61449, false), 9), null, "release");
        TestSupport.eq(enc(e, key(61450, true), 10), "PREV\n", "page-back -> PREV");
        TestSupport.eq(enc(e, key(61450, false), 11), null, "release");
        TestSupport.eq(enc(e, key(99999, true), 12), null, "unmapped key -> null");
        TestSupport.done("NowPlayingEncoderTest");
    }
}
