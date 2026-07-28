package com.besteffortlabs.kindletshell;

import java.awt.Component;
import java.awt.Graphics;
import java.awt.event.KeyEvent;
import java.awt.event.KeyListener;

/**
 * Draw-nothing input surface: forwards every key press/release to the current
 * lease as an OutboundEvent. The LeaseConnection doesn't exist until after
 * connect, so it is injected post-construction via setLease() and snapshotted
 * as a volatile on each key event; a null lease (pre-connect or post-teardown)
 * just drops the key.
 *
 * Home is not forwarded here because it never reaches AWT as a key
 * event on this device -- NowPlayingShell's keyName() table (built from
 * com.amazon.kindle.kindlet.event.KindleKeyCodes) enumerates every code that
 * *does* arrive here (5-way, page bars, menu, back, etc.) and Home is absent
 * from it; its own comment confirms "Home stays unmapped -> still exits",
 * i.e. Home is intercepted below AWT/by the framework. So forwarding every
 * code we actually receive is already correct -- no Home guard is needed, and
 * guessing a code here would be worse than this documented omission. If a
 * device is ever found where Home *does* reach keyPressed, add the explicit
 * `if (code == HOME_CODE) return;` guard the brief describes.
 */
public final class InputRouter extends Component implements KeyListener {

    private volatile LeaseConnection lease;

    public InputRouter() {
        setFocusable(true);
        setFocusTraversalKeysEnabled(false);
        addKeyListener(this);
    }

    public void setLease(LeaseConnection lc) {
        this.lease = lc;
    }

    public void paint(Graphics g) {
        // draw nothing on purpose
    }

    public void keyPressed(KeyEvent e)  { route(e, true); }
    public void keyReleased(KeyEvent e) { route(e, false); }
    public void keyTyped(KeyEvent e)    { e.consume(); }

    private void route(KeyEvent e, boolean pressed) {
        LeaseConnection lc = lease;
        // One line per key edge -> cvm stdout -> /var/log/messages. Confirms keys
        // actually reach this component (focus) and names the code for mapping.
        System.out.println("[SHELL/key] code=" + e.getKeyCode()
                + (pressed ? " down" : " up") + (lc == null ? " (no lease)" : ""));
        OutboundEvent ev = new OutboundEvent(OutboundEvent.KIND_KEY, e.getKeyCode(),
                pressed, (int) System.currentTimeMillis());
        if (lc != null) lc.enqueue(ev);
        e.consume();
    }
}
