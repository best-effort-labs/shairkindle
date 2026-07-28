#!/bin/sh
# Guard: every airplay-* helper the kindlet execs AT RUNTIME must be staged into
# the self-install payload by build-sign.sh -- else it never ships and the feature
# silently no-ops on device (regression: airplay-teardown-resolve was omitted).
set -e
here=$(cd "$(dirname "$0")" && pwd)
bs="$here/../kindlet/build-sign.sh"
for s in airplay-on airplay-off airplay-wifi airplay-lib.sh airplay-nowplaying airplay-teardown-resolve fbink; do
    grep -q "$s" "$bs" || { echo "FAIL: $s not staged by build-sign.sh"; exit 1; }
done
# License texts (MIT/BSD/GPLv3 notices) must ship inside the payload alongside the binaries.
for l in THIRD-PARTY-NOTICES.md SOURCE-OFFER.txt GPL-3.0.txt LLVM-LICENSE.txt; do
    grep -q "$l" "$bs" || { echo "FAIL: license $l not staged into payload by build-sign.sh"; exit 1; }
done
echo "test_payload_bundle OK"
