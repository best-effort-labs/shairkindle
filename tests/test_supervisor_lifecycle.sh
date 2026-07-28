#!/bin/sh
# Integration: run the real supervisor (host build) against fake airplay-on/off +
# a fake raopd. Assert (a) it writes a valid pidfile as session leader, (b) an
# explicit STOP triggers the exit teardown (fake raopd gets TERM), (c) the
# supervisor is one-shot -- it exits on its own after a session ends, and after
# a pre-lease timeout with no client -- and (d) SIGTERM before any session
# still runs the exit-path teardown (airplay-off + pidfile removal), not just
# the session-end path. setsid succeeds only when NOT already a group leader,
# so run under non-interactive sh (no job control) -- the Makefile does.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sup="$here/supervisor-hostcheck"
[ -x "$sup" ] || { echo "SKIP: $sup not built"; exit 0; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"; kill "$svpid" "$sv4" "$sv2" "$sv3" 2>/dev/null || true' EXIT
bin="$tmp/bin"; pre="$tmp/var"; mkdir -p "$bin" "$pre"
# fake airplay-on: record + spawn a fake raopd that writes its own pidfile
cat > "$bin/airplay-on" <<EOF
#!/bin/sh
echo on >> "$tmp/actions.log"
( trap '' HUP; sh -c 'echo \$\$ > "$pre/raopd.pid"; trap "rm -f \"$pre/raopd.pid\"; exit 0" TERM; while :; do sleep 1; done' & )
exit 0
EOF
# fake airplay-off: TERM the fake raopd (honour the pid) + record
cat > "$bin/airplay-off" <<EOF
#!/bin/sh
echo off >> "$tmp/actions.log"
p=\$(cat "$pre/raopd.pid" 2>/dev/null || true)
[ -n "\$p" ] && kill -TERM "\$p" 2>/dev/null || true
exit 0
EOF
chmod +x "$bin/airplay-on" "$bin/airplay-off"

AIRPLAY_BIN="$bin" AIRPLAY_PREFIX="$pre" "$sup" 2>"$tmp/sup.log" &
svpid=$!
# wait for the pidfile (bind + setsid done)
i=0; while [ ! -f "$pre/supervisor.pid" ] && [ $i -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i+1)); done
[ -f "$pre/supervisor.pid" ] || { echo "FAIL: no pidfile"; cat "$tmp/sup.log"; exit 1; }
# pidfile pid matches the process AND is a real session/group leader: PID==PGID==SID
ppid=$(awk '{print $1}' "$pre/supervisor.pid")
[ "$ppid" = "$svpid" ] || { echo "FAIL: pidfile pid $ppid != supervisor $svpid"; exit 1; }
eval "$(awk '{ s=$0; sub(/.*\) /,"",s); split(s,a," "); printf "PGRP=%s SESS=%s\n", a[3], a[4] }' "/proc/$ppid/stat")"
[ "$PGRP" = "$ppid" ] && [ "$SESS" = "$ppid" ] || { echo "FAIL: supervisor not session leader (pgrp=$PGRP sess=$SESS pid=$ppid)"; exit 1; }
# HOLD a lease open (HELLO keep-alive) in the background so airplay-on runs and the
# fake raopd is up; then check inheritance; then STOP. Uses python3 -- swap for a
# tiny C connector (built like tests/rtsp_probe) if the image lacks it.
python3 - <<'PY' &
import socket, time
s = socket.create_connection(("127.0.0.1", 5566))
s.sendall(b"HELLO 1\n")
time.sleep(2)          # hold the lease: raopd is up during the inheritance check
s.sendall(b"STOP\n")
time.sleep(1)
s.close()
PY
client=$!

# the fake raopd (forked via airplay-on's subshell) must INHERIT the supervisor pgid,
# so a single kill(-pgid) reaches it. Wait for it during the held lease, check pgrp.
i=0; while [ ! -f "$pre/raopd.pid" ] && [ $i -lt 30 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i+1)); done
[ -f "$pre/raopd.pid" ] || { echo "FAIL: airplay-on did not start fake raopd"; cat "$tmp/actions.log"; exit 1; }
rpid=$(cat "$pre/raopd.pid")
rpgrp=$(awk '{ s=$0; sub(/.*\) /,"",s); split(s,a," "); print a[3] }' "/proc/$rpid/stat" 2>/dev/null)
[ "$rpgrp" = "$ppid" ] || { echo "FAIL: raopd pgid $rpgrp != supervisor pgid $ppid (group-kill would miss it)"; exit 1; }

wait "$client" 2>/dev/null || true    # lets the STOP land + teardown run
sleep 1
grep -q '^off$' "$tmp/actions.log" || { echo "FAIL: STOP did not trigger airplay-off"; cat "$tmp/actions.log"; exit 1; }
[ ! -f "$pre/raopd.pid" ] || { echo "FAIL: fake raopd not torn down"; exit 1; }

# --- one-shot (STOP path): after STOP tears the session down, the supervisor
# exits ON ITS OWN -- no external kill. (Previously this was asserted only
# after an external SIGTERM, which passed vacuously since STOP's session-end
# teardown had already removed the pidfile.) ---
sleep 1
if kill -0 "$svpid" 2>/dev/null; then echo "FAIL: supervisor stayed resident after STOP"; kill "$svpid" 2>/dev/null; exit 1; fi
[ ! -f "$pre/supervisor.pid" ] || { echo "FAIL: pidfile not removed after one-shot exit"; exit 1; }

# --- exit-path teardown: SIGTERM BEFORE any lease/session must still run
# airplay-off and remove the pidfile -- proving main()'s own SIGTERM-driven
# supervisor_teardown(), not the session-end path. Startup reconciliation
# also logs one "off" line unconditionally, so count invocations (before vs
# after the SIGTERM) rather than just checking presence. ---
rm -f "$pre/supervisor.pid" "$pre/raopd.pid" "$tmp/actions.log"
AIRPLAY_BIN="$bin" AIRPLAY_PREFIX="$pre" "$sup" 2>>"$tmp/sup.log" &
sv4=$!
i=0; while [ ! -f "$pre/supervisor.pid" ] && [ $i -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i+1)); done
[ -f "$pre/supervisor.pid" ] || { echo "FAIL: no pidfile (exit-path test)"; exit 1; }
# wait for the startup-reconcile "off" to land before sampling the baseline
i=0; while [ "$(grep -c '^off$' "$tmp/actions.log" 2>/dev/null || echo 0)" -lt 1 ] && [ $i -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i+1)); done
before=$(grep -c '^off$' "$tmp/actions.log" 2>/dev/null || echo 0)
kill -TERM "$sv4" 2>/dev/null || true
sleep 1
if kill -0 "$sv4" 2>/dev/null; then echo "FAIL: supervisor did not exit on SIGTERM before any session"; kill "$sv4" 2>/dev/null; exit 1; fi
after=$(grep -c '^off$' "$tmp/actions.log" 2>/dev/null || echo 0)
[ "$after" -gt "$before" ] || { echo "FAIL: exit-path teardown did not run airplay-off (before=$before after=$after)"; cat "$tmp/actions.log"; exit 1; }
[ ! -f "$pre/supervisor.pid" ] || { echo "FAIL: pidfile not removed by exit-path teardown"; exit 1; }

# --- one-shot: after a session ends (EOF), the supervisor EXITS (not lazy-resident) ---
rm -f "$pre/supervisor.pid" "$pre/raopd.pid" "$tmp/actions.log"
AIRPLAY_BIN="$bin" AIRPLAY_PREFIX="$pre" "$sup" 2>>"$tmp/sup.log" &
sv2=$!
i=0; while [ ! -f "$pre/supervisor.pid" ] && [ $i -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i+1)); done
# open then immediately close the lease (EOF)
python3 -c 'import socket;socket.create_connection(("127.0.0.1",5566)).close()' 2>/dev/null \
  || (exec 3<>/dev/tcp/127.0.0.1/5566; exec 3>&-) 2>/dev/null || true
sleep 2
if kill -0 "$sv2" 2>/dev/null; then echo "FAIL: supervisor stayed resident after session"; kill "$sv2" 2>/dev/null; exit 1; fi

# --- pre-lease timeout: launched but NEVER connected -> self-exits ---
rm -f "$pre/supervisor.pid"
SUP_PRELEASE_MS=500 AIRPLAY_BIN="$bin" AIRPLAY_PREFIX="$pre" "$sup" 2>>"$tmp/sup.log" &
sv3=$!
sleep 2
if kill -0 "$sv3" 2>/dev/null; then echo "FAIL: unclaimed supervisor did not self-exit"; kill "$sv3" 2>/dev/null; exit 1; fi

echo "test_supervisor_lifecycle OK"
