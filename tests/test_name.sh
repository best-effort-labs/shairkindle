#!/bin/sh
# AIRPLAY_NAME config extraction: read, default when absent, trim trailing ws.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
export AIRPLAY_CONFIG="$work/config"

. "$here/../extension/bin/airplay-lib.sh"

# absent -> default
printf 'WIFI=false\n' > "$AIRPLAY_CONFIG"
[ "$(airplay_name)" = "ShairKindle" ] || { echo "FAIL: default"; exit 1; }

# present
printf 'WIFI=false\nAIRPLAY_NAME=Living Room\n' > "$AIRPLAY_CONFIG"
[ "$(airplay_name)" = "Living Room" ] || { echo "FAIL: read (got '$(airplay_name)')"; exit 1; }

# trailing whitespace trimmed
printf 'AIRPLAY_NAME=Den   \n' > "$AIRPLAY_CONFIG"
[ "$(airplay_name)" = "Den" ] || { echo "FAIL: trim (got '$(airplay_name)')"; exit 1; }

# empty value -> default
printf 'AIRPLAY_NAME=\n' > "$AIRPLAY_CONFIG"
[ "$(airplay_name)" = "ShairKindle" ] || { echo "FAIL: empty->default"; exit 1; }

# missing config file: default, and NO stderr noise
rm -f "$AIRPLAY_CONFIG"
[ "$(airplay_name 2>/dev/null)" = "ShairKindle" ] || { echo "FAIL: missing-file default"; exit 1; }
err=$(airplay_name 2>&1 1>/dev/null)
[ -z "$err" ] || { echo "FAIL: missing-file stderr noise: $err"; exit 1; }

echo "name-config OK"
rm -rf "$work"
