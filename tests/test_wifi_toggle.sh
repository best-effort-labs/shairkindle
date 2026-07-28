#!/bin/sh
# WiFi-toggle logic: config flip is correct + idempotent, and the firewall
# helpers are safe no-ops on a host without iptables (so `make check` is green).
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
export AIRPLAY_CONFIG="$work/config"
export AIRPLAY_PREFIX="$work/var"          # no raopd.pid -> no live firewall path
printf 'WIFI=false\n' > "$AIRPLAY_CONFIG"

. "$here/../extension/bin/airplay-lib.sh"
[ "$(airplay_wifi_on)" = false ] || { echo "FAIL: default not false"; exit 1; }

sh "$here/../extension/bin/airplay-wifi"
grep -q '^WIFI=true$'  "$AIRPLAY_CONFIG" || { echo "FAIL: enable"; exit 1; }
[ "$(airplay_wifi_on)" = true ] || { echo "FAIL: read-back true"; exit 1; }

sh "$here/../extension/bin/airplay-wifi"
grep -q '^WIFI=false$' "$AIRPLAY_CONFIG" || { echo "FAIL: disable"; exit 1; }

# missing config: toggle must create it, defaulting-off -> writes true
rm -f "$AIRPLAY_CONFIG"
sh "$here/../extension/bin/airplay-wifi"
grep -q '^WIFI=false$' "$AIRPLAY_CONFIG" || { echo "FAIL: missing config defaults WiFi on, toggle disables"; exit 1; }

airplay_fw_open; airplay_fw_close          # no-op without iptables, must not error

echo "wifi-toggle OK"
rm -rf "$work"
