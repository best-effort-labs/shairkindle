package com.besteffortlabs.kindletshell;

import java.io.IOException;

/**
 * Assembles and runs the guarded launch line for the native payload. A
 * `ps | grep -q` guard (bracketed on the token's first char so grep
 * doesn't match itself) skips the launch if an instance is already running;
 * otherwise `trap '' HUP` detaches the child so it survives the kindlet exit.
 */
public final class NativeLauncher {

    static String buildCommand(PayloadSpec p) {
        StringBuffer argsJoined = new StringBuffer();
        String[] args = p.args();
        for (int i = 0; i < args.length; i++) {
            argsJoined.append(' ').append(q(args[i]));
        }
        return psGuard(p.psMatchToken()) + " || ( trap '' HUP; "
                + q(p.installDir() + "/" + p.exeRelPath()) + argsJoined
                + " >" + q(p.installDir() + "/" + p.logRelPath()) + " 2>&1 & )";
    }

    /** {@code ps | grep -q <bracketed-token>}: exit 0 iff a matching process runs.
     *  The launch guard and the liveness check (KindletShell.nativeAlive) share this
     *  so they can never disagree on what "already running" means. */
    static String psGuard(String token) {
        // PayloadSpec validates tokens as non-empty alphanumeric strings.
        String bracket = q("[" + token.charAt(0) + "]" + token.substring(1));
        return "ps | grep -q " + bracket;
    }

    /** Single-quote-escape: {@code ' -> '\''} (close-quote, escaped-quote, reopen-quote). */
    private static String q(String s) {
        StringBuffer out = new StringBuffer();
        out.append('\'');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\'') {
                out.append("'\\''");
            } else {
                out.append(c);
            }
        }
        out.append('\'');
        return out.toString();
    }

    /** Runs the guarded launch line on-device via /bin/sh -c. Never throws on exec failure. */
    void launch(PayloadSpec p) {
        try {
            Process proc = Runtime.getRuntime().exec(
                    new String[]{"/bin/sh", "-c", buildCommand(p)});
            proc.waitFor();
        } catch (IOException e) {
            System.err.println("NativeLauncher: exec failed: " + e);
        } catch (InterruptedException e) {
            System.err.println("NativeLauncher: waitFor interrupted: " + e);
        }
    }
}
