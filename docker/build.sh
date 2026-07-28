#!/bin/sh
# Container entrypoint. Mount the repo at /work. Produces the ARM daemon and the
# (signed, if a keystore is available) kindlet into the mounted tree.
set -e
cd /work

echo "== [1/2] cross-build raopd + supervisor (zig, static ARMv6) =="
make raopd-armv6
file out-armv6/raopd
make supervisor-armv6
file out-armv6/airplay-supervisor

echo "== [2/2] build + sign kindlet =="
# Default keystore = the committed public jailbreak keystore. A mounted /keystore
# (for other Kindle generations) overrides it.
if [ -f /keystore ]; then
    echo "  using mounted /keystore"
    KEYSTORE=/keystore sh kindlet/build-sign.sh
elif [ -f kindlet/developer.keystore ]; then
    sh kindlet/build-sign.sh    # build-sign.sh defaults KEYSTORE to kindlet/developer.keystore
else
    echo "  NO keystore (neither /keystore mounted nor kindlet/developer.keystore committed)."
    echo "  Skipping signing. To sign: docker run -v \$PWD:/work -v /path/to.keystore:/keystore <img>"
fi
echo "DONE."
