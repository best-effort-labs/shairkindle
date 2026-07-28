package com.besteffortlabs.kindletshell;

public final class OutboundEvent {
    public static final int KIND_HELLO = 0, KIND_KEY = 1, KIND_HEARTBEAT = 2;
    public final int kind, keyCode, tsMs;
    public final boolean pressed;
    public OutboundEvent(int kind, int keyCode, boolean pressed, int tsMs) {
        this.kind = kind; this.keyCode = keyCode; this.pressed = pressed; this.tsMs = tsMs;
    }
}
