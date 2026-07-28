package com.besteffortlabs.kindletshell;

public class NativeLauncherTest {
    public static void main(String[] a) {
        PayloadSpec p = new PayloadSpec("/var/local/shairkindle", 5566, 10000,
            "airplay-supervisor", new String[]{"-p","5566"}, "supervisor.log", "airplay-super",
            "/var/local/shairkindle/supervisor.pid");
        String cmd = NativeLauncher.buildCommand(p);
        TestSupport.check(cmd.indexOf("'[a]irplay-super'") >= 0, "bracketed ps token: " + cmd);
        TestSupport.check(cmd.indexOf("'/var/local/shairkindle/airplay-supervisor'") >= 0, "abs exe quoted");
        TestSupport.check(cmd.indexOf(">'/var/local/shairkindle/supervisor.log'") >= 0, "log redirect quoted");
        TestSupport.check(cmd.indexOf("trap '' HUP") >= 0, "detached");
        TestSupport.check(cmd.indexOf("'-p' '5566'") >= 0, "args quoted");

        // FIX 2: psMatchToken with an embedded single quote must be shell-escaped, not
        // raw-concatenated. token="a'b-super" -> bracket inner "[a]'b-super" -> escaped.
        PayloadSpec quoteTokenSpec = new PayloadSpec("/var/local/shairkindle", 5566, 10000,
            "airplay-supervisor", new String[]{"-p","5566"}, "supervisor.log", "a'b-super",
            "/var/local/shairkindle/supervisor.pid");
        String quoteTokenCmd = NativeLauncher.buildCommand(quoteTokenSpec);
        TestSupport.check(quoteTokenCmd.indexOf("'[a]'\\''b-super'") >= 0,
            "psMatchToken quote is shell-escaped: " + quoteTokenCmd);

        // Regression guard: an arg with an embedded single quote is already escaped via q().
        PayloadSpec quoteArgSpec = new PayloadSpec("/var/local/shairkindle", 5566, 10000,
            "airplay-supervisor", new String[]{"x'y"}, "supervisor.log", "airplay-super",
            "/var/local/shairkindle/supervisor.pid");
        String quoteArgCmd = NativeLauncher.buildCommand(quoteArgSpec);
        TestSupport.check(quoteArgCmd.indexOf("'x'\\''y'") >= 0,
            "arg quote is shell-escaped: " + quoteArgCmd);

        TestSupport.done("NativeLauncherTest");
    }
}
