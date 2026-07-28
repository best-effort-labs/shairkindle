package com.besteffortlabs.kindletshell;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashMap;
import java.util.StringTokenizer;

/**
 * Manifest-driven self-install of a bundled native payload into an installDir
 * (device: {@code /var/local/<app>/}).
 *
 * PREFIX contract: the payload is the app's ENTIRE on-device footprint under one
 * prefix — binaries, scripts, AND static assets (relpaths may name subdirs, e.g.
 * {@code share/splash.png}; parent dirs are created). The app's build must stage
 * the complete tree and its runtime must resolve paths relative to the prefix,
 * never {@code ../} outside it (the "asset drift" bug class).
 *
 * Manifest text format:
 *   version <opaque-version-string>
 *   <relpath> <sha256-hex> <octal-mode>
 *   ...
 */
public class PayloadInstaller {

    public interface ResourceOpener {
        InputStream open(String relPath);
    }

    public static class Entry {
        public String relPath;
        public String sha256;
        public String mode;
    }

    public static class Manifest {
        public String version;
        public Entry[] entries;
    }

    private static final String STAMP_NAME = ".installed-version";

    /** In-JVM per-installDir lock: canonical installDir path -> monitor object. */
    private static final HashMap installLocks = new HashMap();

    private static Object lockFor(File installDir) {
        String key;
        try {
            key = installDir.getCanonicalPath();
        } catch (IOException e) {
            key = installDir.getAbsolutePath();
        }
        synchronized (installLocks) {
            Object monitor = installLocks.get(key);
            if (monitor == null) {
                monitor = new Object();
                installLocks.put(key, monitor);
            }
            return monitor;
        }
    }

    // --- grammar -----------------------------------------------------------

    public static boolean validRelPath(String p) {
        if (p == null || p.length() == 0) return false;
        if (p.equals(STAMP_NAME)) return false;
        if (p.endsWith(".new")) return false;
        if (p.charAt(0) == '/') return false;
        StringTokenizer st = new StringTokenizer(p, "/", true);
        boolean expectComponent = true;
        while (st.hasMoreTokens()) {
            String tok = st.nextToken();
            if (tok.equals("/")) {
                if (expectComponent) return false; // empty component (leading/double slash)
                expectComponent = true;
                continue;
            }
            if (!validComponent(tok)) return false;
            expectComponent = false;
        }
        if (expectComponent) return false; // trailing slash -> empty final component
        return true;
    }

    private static boolean validComponent(String c) {
        if (c.length() == 0) return false;
        if (c.equals(".") || c.equals("..")) return false;
        for (int i = 0; i < c.length(); i++) {
            char ch = c.charAt(i);
            boolean ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                    || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
            if (!ok) return false;
        }
        return true;
    }

    private static boolean validMode(String m) {
        if (m == null || m.length() < 3 || m.length() > 4) return false;
        for (int i = 0; i < m.length(); i++) {
            char ch = m.charAt(i);
            if (ch < '0' || ch > '7') return false;
        }
        return true;
    }

    private static boolean validSha(String s) {
        if (s == null || s.length() != 64) return false;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            boolean ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            if (!ok) return false;
        }
        return true;
    }

    // --- manifest parsing ----------------------------------------------------

    public static Manifest parseManifest(String text) {
        if (text == null) throw new IllegalArgumentException("null manifest");
        String[] lines = splitLines(text);
        if (lines.length == 0) throw new IllegalArgumentException("empty manifest");

        String versionLine = null;
        int firstEntryIdx = -1;
        for (int i = 0; i < lines.length; i++) {
            String line = lines[i].trim();
            if (line.length() == 0) continue;
            versionLine = line;
            firstEntryIdx = i + 1;
            break;
        }
        if (versionLine == null) throw new IllegalArgumentException("empty manifest");
        if (!versionLine.startsWith("version ")) {
            throw new IllegalArgumentException("first line must be 'version <str>'");
        }
        String version = versionLine.substring("version ".length()).trim();
        if (version.length() == 0) throw new IllegalArgumentException("empty version");

        java.util.ArrayList entries = new java.util.ArrayList();
        java.util.HashMap seen = new java.util.HashMap();
        for (int i = firstEntryIdx; i < lines.length; i++) {
            String line = lines[i].trim();
            if (line.length() == 0) continue;
            StringTokenizer st = new StringTokenizer(line);
            int nFields = st.countTokens();
            if (nFields != 3) {
                throw new IllegalArgumentException("manifest line must have 3 fields: " + line);
            }
            String relPath = st.nextToken();
            String sha = st.nextToken();
            String mode = st.nextToken();

            if (!validRelPath(relPath)) {
                throw new IllegalArgumentException("invalid relPath: " + relPath);
            }
            if (!validSha(sha)) {
                throw new IllegalArgumentException("invalid sha256: " + relPath);
            }
            if (!validMode(mode)) {
                throw new IllegalArgumentException("invalid mode: " + relPath);
            }
            if (seen.containsKey(relPath)) {
                throw new IllegalArgumentException("duplicate relPath: " + relPath);
            }
            seen.put(relPath, Boolean.TRUE);

            Entry e = new Entry();
            e.relPath = relPath;
            e.sha256 = sha;
            e.mode = mode;
            entries.add(e);
        }

        // reject any relPath that is a slash-prefix of another
        for (int i = 0; i < entries.size(); i++) {
            String a = ((Entry) entries.get(i)).relPath;
            for (int j = 0; j < entries.size(); j++) {
                if (i == j) continue;
                String b = ((Entry) entries.get(j)).relPath;
                if (b.startsWith(a + "/")) {
                    throw new IllegalArgumentException(
                            "relPath '" + a + "' is a prefix of '" + b + "'");
                }
            }
        }

        Manifest m = new Manifest();
        m.version = version;
        m.entries = (Entry[]) entries.toArray(new Entry[entries.size()]);
        return m;
    }

    private static String[] splitLines(String text) {
        java.util.ArrayList out = new java.util.ArrayList();
        int start = 0;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) == '\n') {
                out.add(text.substring(start, i));
                start = i + 1;
            }
        }
        if (start < text.length()) out.add(text.substring(start));
        return (String[]) out.toArray(new String[out.size()]);
    }

    // --- sha256 --------------------------------------------------------------

    public static String sha256Hex(byte[] data) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] digest = md.digest(data);
            char[] hex = new char[digest.length * 2];
            String hexChars = "0123456789abcdef";
            for (int i = 0; i < digest.length; i++) {
                int b = digest[i] & 0xff;
                hex[i * 2] = hexChars.charAt(b >> 4);
                hex[i * 2 + 1] = hexChars.charAt(b & 0x0f);
            }
            return new String(hex);
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("SHA-256 unavailable", e);
        }
    }

    private static byte[] readFile(File f) throws IOException {
        int len = (int) f.length();
        byte[] buf = new byte[len];
        FileInputStream in = new FileInputStream(f);
        try {
            int off = 0;
            while (off < len) {
                int n = in.read(buf, off, len - off);
                if (n < 0) break;
                off += n;
            }
        } finally {
            in.close();
        }
        return buf;
    }

    // --- trusted-stamp fast path ----------------------------------------------

    public boolean isUpToDate(File installDir, String version) {
        File stamp = new File(installDir, STAMP_NAME);
        if (!stamp.isFile()) return false;
        try {
            byte[] raw = readFile(stamp);
            String contents = new String(raw, "US-ASCII").trim();
            return contents.equals(version);
        } catch (IOException e) {
            return false;
        }
    }

    // --- install ---------------------------------------------------------------

    public void install(File installDir, Manifest m, ResourceOpener res) throws IOException {
        Object monitor = lockFor(installDir);
        synchronized (monitor) {
            if (!installDir.isDirectory() && !installDir.mkdirs() && !installDir.isDirectory()) {
                throw new IOException("cannot create installDir: " + installDir);
            }
            cleanStaleNew(installDir);

            for (int i = 0; i < m.entries.length; i++) {
                installEntry(installDir, m.entries[i], res);
            }

            // Prune orphans: files from an older version that this manifest no
            // longer ships. Left in place they are dead weight today, but a
            // renamed file plus a fallback path resolver would silently pick up
            // the stale copy. Version is content-derived, so a drop always bumps
            // it -> install() runs -> this prune runs.
            HashMap keep = new HashMap();
            for (int i = 0; i < m.entries.length; i++) {
                keep.put(m.entries[i].relPath, Boolean.TRUE);
            }
            pruneOrphans(installDir, "", keep);

            // stamp written last, staged
            File stamp = new File(installDir, STAMP_NAME);
            File stampNew = new File(installDir, STAMP_NAME + ".new");
            writeFileSynced(stampNew, m.version.getBytes("US-ASCII"));
            renameOrThrow(stampNew, stamp);
        }
    }

    /**
     * Delete any file under dir not named in {@code keep} (relpaths use '/').
     * Fail-closed like the rest of the class: throws on a listing or deletion
     * failure so a lingering stale file can't be masked by a written stamp.
     * Never follows symlinks — an orphan symlink is unlinked, never traversed,
     * so a planted dir-symlink can't escape the tree or cause a delete loop.
     */
    private static void pruneOrphans(File dir, String prefix, HashMap keep) throws IOException {
        File[] files = dir.listFiles();
        if (files == null) throw new IOException("prune: cannot list dir: " + dir);
        for (int i = 0; i < files.length; i++) {
            File f = files[i];
            String rel = prefix.length() == 0 ? f.getName() : prefix + "/" + f.getName();
            if (f.isDirectory() && !isSymlink(f)) {
                pruneOrphans(f, rel, keep);
                // A dir is empty only if its whole subtree was orphaned (the
                // manifest names no dirs); a kept file keeps its parent alive.
                String[] remaining = f.list();
                if (remaining != null && remaining.length == 0 && !f.delete()) {
                    throw new IOException("prune: cannot remove empty dir: " + f);
                }
            } else if (!rel.equals(STAMP_NAME) && !rel.endsWith(".new")
                    && !keep.containsKey(rel)) {
                if (!f.delete()) throw new IOException("prune: cannot delete orphan: " + f);
            }
        }
    }

    /** True if f is a symlink — canonical path differs from parent-canonical + name (old-Java, no NIO). */
    private static boolean isSymlink(File f) throws IOException {
        File parent = f.getParentFile();
        File canonParent = (parent == null) ? f.getCanonicalFile() : parent.getCanonicalFile();
        File resolved = new File(canonParent, f.getName());
        return !resolved.getCanonicalFile().equals(resolved.getAbsoluteFile());
    }

    /**
     * Clear any existing node whose TYPE conflicts with installing {@code relPath}
     * as a regular file: an intermediate path component that is not a real
     * directory (a file/symlink from an older version, which would fail mkdirs),
     * or the target itself being a directory/symlink (which would fail readFile).
     * parseManifest forbids prefix collisions, so such a node is wholly orphaned
     * and safe to remove; without this the install would wedge before prune runs.
     */
    private static void clearTypeConflicts(File installDir, String relPath) throws IOException {
        StringTokenizer st = new StringTokenizer(relPath, "/");
        File node = installDir;
        while (st.hasMoreTokens()) {
            node = new File(node, st.nextToken());
            if (!node.exists()) return; // nothing can exist below a missing node
            boolean last = !st.hasMoreTokens();
            if (last) {
                if (isSymlink(node)) {
                    if (!node.delete()) throw new IOException("clear: cannot unlink target: " + node);
                } else if (node.isDirectory()) {
                    deleteTree(node);
                }
            } else if (isSymlink(node) || !node.isDirectory()) {
                // a file/symlink where a real dir must go — remove it; the rest
                // of the path necessarily didn't exist beneath a non-dir node
                if (!node.delete()) throw new IOException("clear: cannot remove blocker: " + node);
                return;
            }
        }
    }

    /** Recursively delete a real directory subtree; unlink symlinks without traversing. */
    private static void deleteTree(File dir) throws IOException {
        File[] kids = dir.listFiles();
        if (kids != null) {
            for (int i = 0; i < kids.length; i++) {
                File k = kids[i];
                if (k.isDirectory() && !isSymlink(k)) deleteTree(k);
                else if (!k.delete()) throw new IOException("clear: cannot delete: " + k);
            }
        }
        if (!dir.delete()) throw new IOException("clear: cannot remove dir: " + dir);
    }

    private void installEntry(File installDir, Entry entry, ResourceOpener res) throws IOException {
        clearTypeConflicts(installDir, entry.relPath);
        File target = new File(installDir, entry.relPath);
        boolean needsUpdate;
        if (!target.exists()) {
            needsUpdate = true;
        } else {
            String currentSha = sha256Hex(readFile(target));
            needsUpdate = !currentSha.equals(entry.sha256);
        }
        if (!needsUpdate) return;

        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IOException("cannot create parent dir: " + parent);
        }

        File tmp = new File(parent, target.getName() + ".new");
        if (tmp.exists() && !tmp.isFile()) {
            throw new IOException("refusing pre-existing non-regular .new: " + tmp);
        }

        InputStream in = res.open(entry.relPath);
        if (in == null) throw new IOException("resource open returned null: " + entry.relPath);
        byte[] data = readAll(in);

        writeFileSynced(tmp, data);

        String tmpSha = sha256Hex(readFile(tmp));
        if (!tmpSha.equals(entry.sha256)) {
            throw new IOException("sha256 mismatch after extract: " + entry.relPath);
        }

        chmod(entry.mode, tmp);

        renameOrThrow(tmp, target);
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try {
            java.io.ByteArrayOutputStream bout = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) >= 0) {
                bout.write(buf, 0, n);
            }
            return bout.toByteArray();
        } finally {
            in.close();
        }
    }

    private static void writeFileSynced(File f, byte[] data) throws IOException {
        FileOutputStream out = new FileOutputStream(f);
        try {
            out.write(data);
            out.flush();
            FileDescriptor fd = out.getFD();
            fd.sync();
        } finally {
            out.close();
        }
    }

    private static void chmod(String mode, File f) throws IOException {
        String path = f.getAbsolutePath();
        String cmd = "chmod " + mode + " '" + path + "'";
        try {
            Process p = Runtime.getRuntime().exec(
                    new String[]{"/bin/sh", "-c", cmd});
            int rc = p.waitFor();
            if (rc != 0) throw new IOException("chmod failed (" + rc + "): " + path);
        } catch (InterruptedException e) {
            throw new IOException("chmod interrupted: " + path);
        }
    }

    private static void renameOrThrow(File from, File to) throws IOException {
        if (from.renameTo(to)) return;
        String cmd = "mv -f '" + from.getAbsolutePath() + "' '" + to.getAbsolutePath() + "'";
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"/bin/sh", "-c", cmd});
            int rc = p.waitFor();
            if (rc == 0 && to.exists()) return;
        } catch (InterruptedException e) {
            // fall through to throw below
        }
        throw new IOException("rename failed: " + from + " -> " + to);
    }

    private static void cleanStaleNew(File installDir) {
        cleanStaleNewRecursive(installDir);
    }

    private static void cleanStaleNewRecursive(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (int i = 0; i < files.length; i++) {
            File f = files[i];
            if (f.isDirectory()) {
                cleanStaleNewRecursive(f);
            } else if (f.getName().endsWith(".new")) {
                f.delete();
            }
        }
    }
}
