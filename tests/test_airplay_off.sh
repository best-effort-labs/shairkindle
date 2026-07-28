#!/bin/sh
# Unit-test the pid_looks_like_raopd guard (via the AIRPLAY_PROC seam) and the
# airplay-off malformed-pidfile path (must remove the file and kill nothing).
set -e
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../extension/bin/airplay-lib.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# fake /proc: pid 1234 is raopd, pid 5678 is /bin/sh
mkdir -p "$tmp/1234" "$tmp/5678"
ln -s /var/local/shairkindle/raopd "$tmp/1234/exe"
ln -s /bin/sh                  "$tmp/5678/exe"
export AIRPLAY_PROC="$tmp"

pid_looks_like_raopd 1234 || { echo "FAIL: 1234 should be raopd"; exit 1; }
pid_looks_like_raopd 5678 && { echo "FAIL: 5678 should NOT be raopd"; exit 1; }
pid_looks_like_raopd 9999 && { echo "FAIL: missing pid should NOT match"; exit 1; }
pid_looks_like_raopd abc  && { echo "FAIL: non-numeric should NOT match"; exit 1; }

# pid 4321: airplay-on stages an atomic replace of $PREFIX/raopd on every run,
# so a still-running older raopd can read /proc/<pid>/exe as "... (deleted)".
# That must still be accepted, not wrongly rejected.
mkdir -p "$tmp/4321"
ln -s "/var/local/shairkindle/raopd (deleted)" "$tmp/4321/exe"
pid_looks_like_raopd 4321 || { echo "FAIL: 4321 (deleted) should be raopd"; exit 1; }

# malformed pidfile -> airplay-off exits clean and removes it, kills nothing
pdir=$(mktemp -d); trap 'rm -rf "$tmp" "$pdir"' EXIT
printf 'not-a-pid\n' > "$pdir/raopd.pid"
AIRPLAY_PREFIX="$pdir" sh "$here/../extension/bin/airplay-off"
[ -f "$pdir/raopd.pid" ] && { echo "FAIL: malformed pidfile not removed"; exit 1; }

# airplay-on must NOT report success if raopd never publishes a pidfile: a
# self-installed raopd that exits immediately, with AIRPLAY_START_TRIES=1, must
# make airplay-on fail fast (nonzero) rather than hang or return 0.
adir=$(mktemp -d); trap 'rm -rf "$tmp" "$pdir" "$adir"' EXIT
mkdir -p "$adir/var" "$adir/proc"
cat > "$adir/var/raopd" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$adir/var/raopd"
if AIRPLAY_PREFIX="$adir/var" \
   AIRPLAY_PROC="$adir/proc" AIRPLAY_START_TRIES=1 \
   sh "$here/../extension/bin/airplay-on"; then
    echo "FAIL: airplay-on should fail when raopd never publishes a pidfile"
    exit 1
fi

echo "test_airplay_off OK"
