package com.besteffortlabs.kindletshell;

public final class PayloadSpec {
    private final String installDir, exeRelPath, logRelPath, psMatchToken, pidfilePath;
    private final String[] args; private final int port, heartbeatMs;
    public PayloadSpec(String installDir, int port, int heartbeatMs, String exeRelPath,
                       String[] args, String logRelPath, String psMatchToken, String pidfilePath) {
        this.installDir = installDir; this.port = port; this.heartbeatMs = heartbeatMs;
        this.exeRelPath = exeRelPath; this.args = args; this.logRelPath = logRelPath;
        this.psMatchToken = psMatchToken; this.pidfilePath = pidfilePath;
    }
    public String installDir()   { return installDir; }
    public int port()            { return port; }
    public int heartbeatMs()     { return heartbeatMs; }
    public String exeRelPath()   { return exeRelPath; }
    public String[] args()       { return args; }
    public String logRelPath()   { return logRelPath; }
    public String psMatchToken() { return psMatchToken; }
    public String pidfilePath()  { return pidfilePath; }
}
