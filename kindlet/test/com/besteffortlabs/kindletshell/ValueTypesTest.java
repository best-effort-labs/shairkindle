package com.besteffortlabs.kindletshell;

public class ValueTypesTest {
    public static void main(String[] args) {
        OutboundEvent e = new OutboundEvent(OutboundEvent.KIND_KEY, 61451, true, 123);
        TestSupport.check(e.kind == 1 && e.keyCode == 61451 && e.pressed && e.tsMs == 123, "event fields");

        String[] a = new String[] { "-p", "5566" };
        PayloadSpec p = new PayloadSpec("/var/local/shairkindle", 5566, 10000,
                "airplay-supervisor", a, "supervisor.log", "airplay-super",
                "/var/local/shairkindle/supervisor.pid");
        TestSupport.eq(p.installDir(), "/var/local/shairkindle", "installDir");
        TestSupport.check(p.port() == 5566 && p.heartbeatMs() == 10000, "port/hb");
        TestSupport.eq(p.exeRelPath(), "airplay-supervisor", "exe");
        TestSupport.eq(p.psMatchToken(), "airplay-super", "token");
        TestSupport.check(p.args().length == 2, "args len");
        TestSupport.done("ValueTypesTest");
    }
}
