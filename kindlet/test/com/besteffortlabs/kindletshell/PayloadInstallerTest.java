package com.besteffortlabs.kindletshell;

import java.io.*;

public class PayloadInstallerTest {
    public static void main(String[] a) throws Exception {
        testGrammar();
        testParseRejectsDupAndReserved();
        testParseRejectsSlashPrefix();
        testInstallAndStampFastPath();
        testSelfHealRepairsDeletedFile();
        testInstallRejectsShaMismatchBeforeSwap();
        testPrunesOrphanedFiles();
        testHandlesFileDirTypeSwaps();
        TestSupport.done("PayloadInstallerTest");
    }

    static void testGrammar() {
        TestSupport.check(PayloadInstaller.validRelPath("bin/airplay-supervisor"), "ok path");
        TestSupport.check(!PayloadInstaller.validRelPath("/abs"), "reject absolute");
        TestSupport.check(!PayloadInstaller.validRelPath("a/../b"), "reject ..");
        TestSupport.check(!PayloadInstaller.validRelPath(".installed-version"), "reject reserved");
        TestSupport.check(!PayloadInstaller.validRelPath("x.new"), "reject .new");
        TestSupport.check(!PayloadInstaller.validRelPath("a//b"), "reject empty comp");
    }

    static void testParseRejectsDupAndReserved() {
        boolean threw = false;
        try { PayloadInstaller.parseManifest("version v1\nx abcd 755\nx abcd 755\n"); }
        catch (IllegalArgumentException e) { threw = true; }
        TestSupport.check(threw, "duplicate relpath rejected");
    }

    static void testParseRejectsSlashPrefix() {
        String sha = PayloadInstaller.sha256Hex("x".getBytes());
        boolean threw = false;
        try {
            PayloadInstaller.parseManifest(
                    "version v1\nbin " + sha + " 755\nbin/tool " + sha + " 755\n");
        } catch (IllegalArgumentException e) { threw = true; }
        TestSupport.check(threw, "slash-prefix relpath rejected");
    }

    static void testInstallAndStampFastPath() throws Exception {
        File dir = File.createTempFile("pi_", "d"); dir.delete(); dir.mkdirs(); // prefix must be >=3 chars (JDK ctr)
        final byte[] payload = "hello-binary".getBytes("US-ASCII");
        String sha = PayloadInstaller.sha256Hex(payload);
        final PayloadInstaller.Manifest m = PayloadInstaller.parseManifest("version v1\nbin/tool " + sha + " 755\n");
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String rel) { return new ByteArrayInputStream(payload); }
        };
        PayloadInstaller pi = new PayloadInstaller();
        TestSupport.check(!pi.isUpToDate(dir, "v1"), "not installed yet");
        pi.install(dir, m, res);
        File out = new File(dir, "bin/tool");
        TestSupport.check(out.exists() && out.length() == payload.length, "file installed");
        TestSupport.check(pi.isUpToDate(dir, "v1"), "stamp written -> up to date");
        // trusted-stamp fast path: same version reports up to date without re-diffing
        TestSupport.check(pi.isUpToDate(dir, "v1"), "trusted stamp fast-path");
        // new version -> not up to date -> caller should re-diff
        TestSupport.check(!pi.isUpToDate(dir, "v2"), "new version triggers re-diff");
    }

    static void testSelfHealRepairsDeletedFile() throws Exception {
        File dir = File.createTempFile("pi_", "d"); dir.delete(); dir.mkdirs();
        final byte[] payload = "self-heal-binary".getBytes("US-ASCII");
        String sha = PayloadInstaller.sha256Hex(payload);
        final PayloadInstaller.Manifest m = PayloadInstaller.parseManifest("version v1\nbin/tool " + sha + " 755\n");
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String rel) { return new ByteArrayInputStream(payload); }
        };
        PayloadInstaller pi = new PayloadInstaller();
        pi.install(dir, m, res);
        File out = new File(dir, "bin/tool");
        TestSupport.check(out.exists() && out.length() == payload.length, "file installed first time");
        TestSupport.check(pi.isUpToDate(dir, "v1"), "stamp matches after first install");

        // simulate a damaged tree: delete the installed payload file, leave the stamp intact
        TestSupport.check(out.delete(), "deleted installed file to simulate damage");
        TestSupport.check(!out.exists(), "file really gone before repair");

        // re-run install with the SAME version -> must re-extract the missing file
        pi.install(dir, m, res);
        TestSupport.check(out.exists() && out.length() == payload.length, "self-heal recreated the file");
        byte[] repaired = new byte[(int) out.length()];
        FileInputStream in = new FileInputStream(out);
        try { in.read(repaired); } finally { in.close(); }
        TestSupport.eq(PayloadInstaller.sha256Hex(repaired), sha, "repaired file content matches manifest sha");
    }

    static void testPrunesOrphanedFiles() throws Exception {
        File dir = File.createTempFile("pi_", "d"); dir.delete(); dir.mkdirs();
        final byte[] payload = "shared-binary".getBytes("US-ASCII");
        final String sha = PayloadInstaller.sha256Hex(payload);
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String rel) { return new ByteArrayInputStream(payload); }
        };
        PayloadInstaller pi = new PayloadInstaller();

        // v1 ships a nested kept file, a top-level orphan-to-be, and a nested orphan-to-be
        pi.install(dir, PayloadInstaller.parseManifest(
                "version v1\nbin/keep " + sha + " 755\nold-tool " + sha + " 755\n"
                        + "share/old.png " + sha + " 644\n"), res);
        TestSupport.check(new File(dir, "bin/keep").exists(), "v1 kept file present");
        TestSupport.check(new File(dir, "old-tool").exists(), "v1 top-level orphan-to-be present");
        TestSupport.check(new File(dir, "share/old.png").exists(), "v1 nested orphan-to-be present");

        // v2 keeps only bin/keep -> both orphans go, empty share/ goes, bin/ survives
        pi.install(dir, PayloadInstaller.parseManifest(
                "version v2\nbin/keep " + sha + " 755\n"), res);
        TestSupport.check(new File(dir, "bin/keep").exists(), "kept file survives prune");
        TestSupport.check(new File(dir, "bin").isDirectory(), "dir holding a kept file survives");
        TestSupport.check(!new File(dir, "old-tool").exists(), "top-level orphan pruned");
        TestSupport.check(!new File(dir, "share/old.png").exists(), "nested orphan pruned");
        TestSupport.check(!new File(dir, "share").exists(), "emptied orphan dir removed");
        TestSupport.check(new File(dir, ".installed-version").isFile(), "stamp not pruned");
    }

    static void testHandlesFileDirTypeSwaps() throws Exception {
        final byte[] payload = "swap-bytes".getBytes("US-ASCII");
        final String sha = PayloadInstaller.sha256Hex(payload);
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String rel) { return new ByteArrayInputStream(payload); }
        };
        PayloadInstaller pi = new PayloadInstaller();

        // file -> dir: v1 ships file `node`, v2 ships `node/child` (the file blocks mkdirs)
        File d1 = File.createTempFile("pi_", "d"); d1.delete(); d1.mkdirs();
        pi.install(d1, PayloadInstaller.parseManifest("version v1\nnode " + sha + " 755\n"), res);
        TestSupport.check(new File(d1, "node").isFile(), "v1 node is a file");
        pi.install(d1, PayloadInstaller.parseManifest("version v2\nnode/child " + sha + " 755\n"), res);
        TestSupport.check(new File(d1, "node").isDirectory(), "node became a dir");
        TestSupport.check(new File(d1, "node/child").isFile(), "node/child installed after swap");

        // dir -> file: v1 ships `node/child`, v2 ships file `node` (the dir blocks readFile)
        File d2 = File.createTempFile("pi_", "d"); d2.delete(); d2.mkdirs();
        pi.install(d2, PayloadInstaller.parseManifest("version v1\nnode/child " + sha + " 755\n"), res);
        TestSupport.check(new File(d2, "node/child").isFile(), "v1 node/child is a file");
        pi.install(d2, PayloadInstaller.parseManifest("version v2\nnode " + sha + " 755\n"), res);
        TestSupport.check(new File(d2, "node").isFile(), "node became a file after swap");
        TestSupport.check(!new File(d2, "node/child").exists(), "old node/child cleared");
    }

    static void testInstallRejectsShaMismatchBeforeSwap() throws Exception {
        File dir = File.createTempFile("pi_", "d"); dir.delete(); dir.mkdirs();
        final byte[] goodBytes = "good-bytes".getBytes("US-ASCII");
        final byte[] badBytes = "totally-different-bytes".getBytes("US-ASCII");
        String goodSha = PayloadInstaller.sha256Hex(goodBytes);
        final PayloadInstaller.Manifest m = PayloadInstaller.parseManifest("version v1\nbin/tool " + goodSha + " 755\n");
        PayloadInstaller.ResourceOpener res = new PayloadInstaller.ResourceOpener() {
            public InputStream open(String rel) { return new ByteArrayInputStream(badBytes); }
        };
        PayloadInstaller pi = new PayloadInstaller();
        boolean threw = false;
        try { pi.install(dir, m, res); }
        catch (IOException e) { threw = true; }
        TestSupport.check(threw, "sha256 mismatch before swap throws");
        File out = new File(dir, "bin/tool");
        TestSupport.check(!out.exists(), "corrupt payload not swapped into place");
    }
}
