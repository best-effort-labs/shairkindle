#!/bin/sh
# Debug-logging opt-in: OFF by default (so a clean device never accumulates the
# raopd stream-stats firehose and fills /var/local), ON only when the sentinel
# file exists. Guards the release-robustness default against a regression.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export AIRPLAY_DEBUG_FLAG="$work/shairkindle-debug"

. "$here/../extension/bin/airplay-lib.sh"

# absent sentinel -> off (raopd log goes to /dev/null)
[ "$(airplay_debug_on)" = false ] || { echo "FAIL: default not off"; exit 1; }

# present sentinel -> on
: > "$AIRPLAY_DEBUG_FLAG"
[ "$(airplay_debug_on)" = true ] || { echo "FAIL: sentinel present not on"; exit 1; }

# removed again -> back off (idempotent read, no state)
rm -f "$AIRPLAY_DEBUG_FLAG"
[ "$(airplay_debug_on)" = false ] || { echo "FAIL: removed sentinel not off"; exit 1; }

echo "debug-log OK"
