package com.besteffortlabs.kindletshell;

public final class TestSupport {
    private static int checks = 0;
    public static void check(boolean cond, String msg) {
        checks++;
        if (!cond) { throw new RuntimeException("CHECK FAILED: " + msg); }
    }
    public static void eq(Object a, Object b, String msg) {
        check(a == null ? b == null : a.equals(b), msg + " (got " + a + ", want " + b + ")");
    }
    public static void done(String suite) {
        System.out.println("OK " + suite + " (" + checks + " checks)");
        checks = 0;
    }
}
