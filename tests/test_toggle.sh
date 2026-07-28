#!/bin/sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
export AIRPLAY_PREFIX="$work/var"
export AIRPLAY_PROC="$work/proc"
# fake raopd: publishes its own pidfile (like the real one) then sleeps, so the
# launcher must NOT write the pidfile itself. Self-installed directly at
# $AIRPLAY_PREFIX/raopd -- airplay-on no longer stages it (the kindlet shell's
# PayloadInstaller does that before the supervisor ever runs this script).
mkdir -p "$AIRPLAY_PREFIX" "$AIRPLAY_PROC"
cat > "$AIRPLAY_PREFIX/raopd" <<'EOF'
#!/bin/sh
pid=$$
echo "$pid" > "$AIRPLAY_PREFIX/raopd.pid"
# Set up fake /proc entry so pid_looks_like_raopd recognizes this process
mkdir -p "$AIRPLAY_PROC/$pid"
ln -s /var/local/shairkindle/raopd "$AIRPLAY_PROC/$pid/exe"
exec sleep 300
EOF
chmod +x "$AIRPLAY_PREFIX/raopd"
export AIRPLAY_START_TRIES=3
sh "$here/../extension/bin/airplay-on"
test -x "$AIRPLAY_PREFIX/raopd"            # self-installed, executable
for i in 1 2 3 4 5; do [ -f "$AIRPLAY_PREFIX/raopd.pid" ] && break; sleep 1; done
test -f "$AIRPLAY_PREFIX/raopd.pid"        # pid published (by the daemon)
pid=$(cat "$AIRPLAY_PREFIX/raopd.pid")
kill -0 "$pid"                             # running
sh "$here/../extension/bin/airplay-off"
sleep 1
if kill -0 "$pid" 2>/dev/null; then echo "FAIL: still running"; exit 1; fi
echo "toggle OK"
rm -rf "$work"
