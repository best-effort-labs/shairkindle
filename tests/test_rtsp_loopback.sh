#!/bin/sh
# Host loopback smoke test: launch `raopd --smoke <port>` (foreground, no
# daemonize, no mDNS), then drive it with rtsp_probe over loopback. Asserts the
# OPTIONS round-trip AND the nonzero SETUP server_port (RTP port-ordering).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

raopd="$root/tests/smoke-raopd"
probe="$here/rtsp_probe"
port=45123

cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

"$raopd" --smoke "$port" &
pid=$!

# Wait for the listener (bounded): retry the probe until it connects or we give up.
i=0
while [ $i -lt 50 ]; do
    if "$probe" "$port"; then
        cleanup
        trap - EXIT
        echo "test_rtsp_loopback OK"
        exit 0
    fi
    kill -0 "$pid" 2>/dev/null || { echo "FAIL: raopd exited early"; exit 1; }
    i=$((i + 1))
    sleep 0.1
done

echo "FAIL: probe never succeeded"
exit 1
