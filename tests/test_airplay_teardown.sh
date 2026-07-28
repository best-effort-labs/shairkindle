#!/bin/sh
# airplay-teardown-resolve: validates the supervisor pidfile against a fake /proc
# (exe name + PID==PGID==SID + starttime token) and prints the group-leader pid.
# Also proves the raw group-kill primitive reaps a TERM-ignoring member.
set -e
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../extension/bin/airplay-lib.sh"

tmp=$(mktemp -d); trap 'rm -rf "$tmp" "$fbdir" "$empty"' EXIT
proc="$tmp/proc"; pre="$tmp/var"
mkdir -p "$proc/4000" "$pre"
# fake supervisor: pid 4000, session leader (pgrp==session==pid), starttime 555
ln -s /var/local/shairkindle/airplay-supervisor "$proc/4000/exe"
printf '4000 (airplay-supervisor) S 1 4000 4000 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 555 rest\n' > "$proc/4000/stat"
export AIRPLAY_PROC="$proc" AIRPLAY_PREFIX="$pre"

# valid pidfile -> resolver prints the pid
printf '4000 555\n' > "$pre/supervisor.pid"
out=$(sh "$here/../extension/bin/airplay-teardown-resolve")
[ "$out" = 4000 ] || { echo "FAIL: valid pidfile should resolve to 4000 (got '$out')"; exit 1; }

# starttime mismatch (PID reuse) -> reject
printf '4000 999\n' > "$pre/supervisor.pid"
out=$(sh "$here/../extension/bin/airplay-teardown-resolve")
[ -z "$out" ] || { echo "FAIL: starttime mismatch must reject (got '$out')"; exit 1; }

# not a session leader (pgrp!=pid) -> reject
printf '4000 (airplay-supervisor) S 1 3999 3999 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 555 rest\n' > "$proc/4000/stat"
printf '4000 555\n' > "$pre/supervisor.pid"
out=$(sh "$here/../extension/bin/airplay-teardown-resolve")
[ -z "$out" ] || { echo "FAIL: non-leader must reject (got '$out')"; exit 1; }

# missing pidfile -> empty, exit 0 (nothing to tear down)
rm -f "$pre/supervisor.pid"
out=$(sh "$here/../extension/bin/airplay-teardown-resolve")
[ -z "$out" ] || { echo "FAIL: missing pidfile must resolve empty (got '$out')"; exit 1; }

# --- raw group-kill primitive: a TERM-ignoring member needs SIGKILL ---
# setsid a group leader that ignores TERM; TERM it (survives), then KILL the group.
setsid sh -c 'trap "" TERM; exec sleep 30' &
leader=$!
sleep 1
kill -TERM "-$leader" 2>/dev/null || true
sleep 1
kill -0 "-$leader" 2>/dev/null || { echo "FAIL: TERM-ignoring group should still be alive"; exit 1; }
kill -KILL "-$leader" 2>/dev/null || true
sleep 1
if kill -0 "-$leader" 2>/dev/null; then echo "FAIL: group survived SIGKILL"; exit 1; fi

# --- ps fallback: uses REAL ps + REAL /proc (not the fakes above) ---
unset AIRPLAY_PROC
fbdir=$(mktemp -d)
cp "$(command -v sleep)" "$fbdir/airplay-supervisor"
empty=$(mktemp -d)   # no supervisor.pid -> forces the ps fallback

# fallback prints a validated leader (setsid: PID==PGID==SID)
setsid "$fbdir/airplay-supervisor" 30 &
leaderpid=$!
sleep 1
out=$(AIRPLAY_PREFIX="$empty" sh "$here/../extension/bin/airplay-teardown-resolve")
kill "$leaderpid" 2>/dev/null || true
wait "$leaderpid" 2>/dev/null || true
[ "$out" = "$leaderpid" ] || { echo "FAIL: ps fallback should print the validated leader (got '$out' want '$leaderpid')"; exit 1; }

# fallback rejects a non-leader match (no setsid -> inherits this shell's pgid)
"$fbdir/airplay-supervisor" 30 &
nonleaderpid=$!
sleep 1
out=$(AIRPLAY_PREFIX="$empty" sh "$here/../extension/bin/airplay-teardown-resolve")
kill "$nonleaderpid" 2>/dev/null || true
wait "$nonleaderpid" 2>/dev/null || true
[ -z "$out" ] || { echo "FAIL: non-leader ps match must be rejected (got '$out')"; exit 1; }

echo "test_airplay_teardown OK"
