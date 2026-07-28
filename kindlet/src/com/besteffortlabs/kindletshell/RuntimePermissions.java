package com.besteffortlabs.kindletshell;

import ixtab.jailbreak.Jailbreak;
import java.security.AccessController;
import java.security.AllPermission;

public final class RuntimePermissions {
    private Jailbreak jailbreak;
    private boolean enabledByUs;

    /**
     * Obtain the permissions needed by this Kindlet. Keep the same Jailbreak
     * frontend for the lifetime of the Kindlet so destroy() can restore the
     * runtime policy wrapper if this instance was the one that enabled it.
     */
    public synchronized boolean acquire() {
        try {
            if (jailbreak == null) jailbreak = new Jailbreak();
            Jailbreak jb = jailbreak;
            if (!jb.isAvailable()) { log("no backend"); return false; }
            if (!jb.isEnabled()) {
                if (!jb.enable()) { log("enable failed"); return false; }
                enabledByUs = true;
            }
            jb.getContext().requestPermission(new AllPermission());
            AccessController.checkPermission(new AllPermission());   // fail-closed
            log("runtime permissions granted");
            return true;
        } catch (Throwable t) {
            log("runtime permission request failed: " + t);
            release();
            return false;
        }
    }

    /**
     * Restore the prior runtime policy state, but only when this bootstrap
     * enabled the wrapper. If another Kindlet had already enabled it, ownership
     * remains with that Kindlet and we leave it alone. Idempotent.
     */
    public synchronized void release() {
        if (!enabledByUs || jailbreak == null) return;
        try {
            if (jailbreak.disable()) {
                enabledByUs = false;
                log("released");
            } else {
                log("release failed");
            }
        } catch (Throwable t) {
            log("release failed: " + t);
        }
    }
    private void log(String s) { System.out.println("[SHELL/priv] " + s); System.out.flush(); }
}
