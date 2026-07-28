#!/bin/sh
# airplay-on must ALWAYS reconcile the wifi power mode from config: a
# dirty 'maxperf' left by a prior forced SIGKILL must be reset. With WIFI=false
# the run must invoke wmiconfig --power rec (powersave), not leave maxperf.
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/var" "$tmp/bin"
# capturing fake wmiconfig
cat > "$tmp/bin/wmiconfig" <<EOF
#!/bin/sh
echo "\$@" >> "$tmp/wmi.log"
EOF
chmod +x "$tmp/bin/wmiconfig"
# a raopd that comes up and publishes its pidfile so airplay-on exits 0 fast.
# pid_looks_like_raopd() requires /proc/<pid>/exe to end in "raopd" -- `exec
# sleep 30` would leave it pointing at the real sleep binary instead, so exec
# into a copy of `sleep` renamed to `raopd` (basename is what's matched).
mkdir -p "$tmp/var/real"
cp "$(command -v sleep)" "$tmp/var/real/raopd"
chmod +x "$tmp/var/real/raopd"
cat > "$tmp/var/raopd" <<EOF
#!/bin/sh
echo \$\$ > "$tmp/var/raopd.pid"
exec "$tmp/var/real/raopd" 30
EOF
chmod +x "$tmp/var/raopd"
mkdir -p "$tmp/proc/$$"   # not used; airplay-on checks its own child via AIRPLAY_PROC

printf 'WIFI=false\n' > "$tmp/config"
AIRPLAY_CONFIG="$tmp/config" AIRPLAY_PREFIX="$tmp/var" \
  AIRPLAY_WMICONFIG="$tmp/bin/wmiconfig" AIRPLAY_WLAN=wlan0 \
  AIRPLAY_PROC=/proc AIRPLAY_START_TRIES=5 \
  sh "$here/../extension/bin/airplay-on" || true
kill "$(cat "$tmp/var/raopd.pid" 2>/dev/null)" 2>/dev/null || true

grep -q -- '--power rec' "$tmp/wmi.log" \
  || { echo "FAIL: WIFI=false did not reconcile to powersave"; cat "$tmp/wmi.log" 2>/dev/null; exit 1; }
echo "test_startup_reconcile OK"
