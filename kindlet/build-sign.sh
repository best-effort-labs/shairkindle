#!/bin/sh
# Build + cvm-sign the shairkindle now-playing shell kindlet (.azw2).
#
# RUNS ON A JDK-8 HOST (this Mac has no Java runtime). Self-contained: compiles
# against the clean-room API stubs in stubs/ (our code) -- NO proprietary Amazon
# jar required. The stub classes are used only at compile time and are NOT
# bundled into the .azw2; the Kindle framework supplies the real classes at
# runtime.
#
# Signing is GENERATION-SPECIFIC. This K3 (fw 3.4.3) uses RSA/SHA-256; the K2
# used DSA/SHA1. The default is the committed, publicly distributed jailbreak
# developer keystore used by KUAL-era Kindlets. Override KEYSTORE only when the
# target has a different compatible developer-certificate set. Confirm the
# algorithm from a working app's META-INF (.RSA vs .DSA; SHA-256- vs
# SHA1-Digest-Manifest). See README.md.
#
# Inputs (env overridable):
#   KEYSTORE     compatible developer keystore (default: committed public
#                jailbreak keystore; its private keys are already public)
#   STOREPASS    keystore password                       (default "password")
#   SIGALG/DIGESTALG  signature scheme        (default SHA256withRSA / SHA-256)
#   SRC/TGT      ecj/javac source/target level                 (default 1.4)
#   KINDLET_JAR  OPTIONAL: compile against the real on-device Kindlet API jar
#                instead of stubs/ (normally unnecessary)
# Output: out/ShairKindle.azw2
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

KEYSTORE="${KEYSTORE:-$here/developer.keystore}"
STOREPASS="${STOREPASS:-password}"
SRC="${SRC:-1.4}"; TGT="${TGT:-1.4}"
SIGALG="${SIGALG:-SHA256withRSA}"
DIGESTALG="${DIGESTALG:-SHA-256}"
ECJ_JAR="${ECJ_JAR:-/usr/share/java/ecj.jar}"
APP=out/ShairKindle.azw2

[ -f "$KEYSTORE" ] || { echo "FATAL: keystore not found: $KEYSTORE"; exit 1; }

# ecj emits true 1.4 bytecode on a JDK-8 host (javac floor is 1.5); the packaged
# `ecj` wrapper needs java-wrappers.sh, so invoke the jar's main class directly.
compile() {   # $1=outdir; rest = javac-style args incl. sources
    out="$1"; shift
    if [ -f "$ECJ_JAR" ]; then
        java -cp "$ECJ_JAR" org.eclipse.jdt.internal.compiler.batch.Main \
            -source "$SRC" -target "$TGT" -nowarn -d "$out" "$@"
    else
        echo "  (ecj not found; javac at 1.5 -> major-49; verify cvm accepts it)"
        javac -source 1.5 -target 1.5 -d "$out" "$@"
    fi
}

rm -rf classes stub-classes out payload; mkdir -p classes out

if [ -n "$KINDLET_JAR" ] && [ -f "$KINDLET_JAR" ]; then
    echo "[1/7] compile app against real API jar: $KINDLET_JAR"
    CP="$KINDLET_JAR"
else
    echo "[1/7a] compile clean-room API stubs (incl. the ParseException gateway stub)"
    mkdir -p stub-classes
    compile stub-classes $(find stubs -name '*.java')
    echo "[1/7b] compile app against stubs"
    CP="stub-classes"
fi
# whole tree: kindletshell (shell library) + shairkindle (the ported app) + ixtab/jailbreak
compile classes -classpath "$CP" $(find src -name '*.java')
major=$(od -An -tu1 -j7 -N1 classes/com/besteffortlabs/shairkindle/ShairKindle.class | tr -d ' ')
echo "  bytecode major: $major  (48=1.4, 49=5, 50=6)"
[ "$major" = 48 ] || { echo "FATAL: bytecode major $major != 48 (device cvm requires 48; is ecj available?)"; exit 1; }

echo "[2/7] stage self-install payload (flat -> /var/local/shairkindle)"
PAYLOAD=payload
rm -rf "$PAYLOAD"; mkdir -p "$PAYLOAD"
cp ../out-armv6/raopd              "$PAYLOAD/raopd"
cp ../out-armv6/airplay-supervisor "$PAYLOAD/airplay-supervisor"
for s in airplay-on airplay-off airplay-wifi airplay-lib.sh airplay-nowplaying airplay-teardown-resolve; do
    cp "../extension/bin/$s" "$PAYLOAD/$s"
done
# Bundled fbink (GPLv3, exec'd never linked) so the display works without usbnet; lands
# flat at /var/local/shairkindle/fbink, resolved by airplay-nowplaying at $here/fbink. Manifest
# walk picks it up as 755 (no .sh/.png suffix). See SOURCE-OFFER.txt.
cp ../extension/bin/fbink "$PAYLOAD/fbink"
# Static render assets -> payload/share/ (installs UNDER the app PREFIX as
# /var/local/shairkindle/share/*, resolved by airplay-nowplaying at $here/share).
# PayloadInstaller creates the subdir; the manifest walk below picks them up.
mkdir -p "$PAYLOAD/share"
cp ../extension/share/splash.png ../extension/share/art-placeholder.png "$PAYLOAD/share/"

# License texts travel WITH the distributed binaries: the payload statically links
# MIT/BSD/LLVM components and bundles the GPLv3 fbink, whose notices must accompany the
# binary (MIT "all copies", BSD-3 clause 2, GPLv3 §4/§6). Installed to
# /var/local/shairkindle/licenses/ so they ship inside the azw2, not just in the source repo.
mkdir -p "$PAYLOAD/licenses"
cp ../THIRD-PARTY-NOTICES.md ../SOURCE-OFFER.txt "$PAYLOAD/licenses/"
cp ../licenses/GPL-3.0.txt ../licenses/LLVM-LICENSE.txt "$PAYLOAD/licenses/"

# manifest: version = first 16 hex of sha256(concatenated per-file sha256s) so any
# payload change bumps it; then one line per file "relpath sha256 octal-mode".
# .sh (sourced lib) + data assets = 644; binaries + exec scripts = 755.
{
  printf 'version '
  ( cd "$PAYLOAD" && find . -type f ! -name manifest | LC_ALL=C sort | while read f; do sha256sum "$f"; done ) | sha256sum | cut -c1-16
  echo
  ( cd "$PAYLOAD" && find . -type f ! -name manifest | LC_ALL=C sort | while read f; do
      rel=${f#./}; h=$(sha256sum "$f" | cut -d' ' -f1)
      case "$rel" in *.sh|*.png|*.md|*.txt) mode=644;; *) mode=755;; esac
      echo "$rel $h $mode"
    done )
} > "$PAYLOAD/manifest"

echo "[3/7] cover.png (plain placeholder for the Home carousel)"
python3 - cover.png <<'PY'
import zlib, struct, sys
w, h = 300, 400
raw = bytearray()
for y in range(h):
    raw.append(0)                       # PNG filter type 0 per scanline
    raw += bytes([0xcc, 0xcc, 0xcc]) * w
def chunk(t, d):
    b = t + d
    return struct.pack(">I", len(d)) + b + struct.pack(">I", zlib.crc32(b) & 0xffffffff)
png  = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
png += chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY

echo "[4/7] assemble $APP (classes + payload -- stubs excluded)"
jar cfm "$APP" META-INF/MANIFEST.MF cover.png -C classes . payload

echo "[5/7] guard: ParseException must not ship (device's patched copy is the gateway)"
if unzip -l "$APP" | grep -q 'org/json/simple/parser/ParseException'; then
    echo "FATAL: ParseException must NOT be bundled (device copy is the gateway)"; exit 1
fi

echo "[6/7] sign ($SIGALG / $DIGESTALG, aliases ditest dktest dntest)"
# nolimits.props re-enables SHA-1 signing (only needed for the DSA/SHA1 path on
# newer JDKs); harmless for RSA/SHA-256.
cat > nolimits.props <<'EOF'
jdk.certpath.disabledAlgorithms=
jdk.jar.disabledAlgorithms=
jdk.security.legacyAlgorithms=
EOF
for a in ditest dktest dntest; do
    jarsigner -keystore "$KEYSTORE" -storepass "$STOREPASS" -keypass "$STOREPASS" \
        -digestalg "$DIGESTALG" -sigalg "$SIGALG" \
        -J-Djava.security.properties="$here/nolimits.props" \
        "$APP" "$a"
done
rm -f nolimits.props

echo "[7/7] verify (necessary, not sufficient -- device launch is the real gate)"
jarsigner -verify -keystore "$KEYSTORE" "$APP" 2>&1 | grep -i "verified" || echo "  (verify: see jarsigner output)"
unzip -p "$APP" META-INF/DITEST.SF | grep -i "Digest-Manifest:" | head -1 | sed 's/^/  SF header: /'
echo "  (must match the working installed KUAL scheme for the target generation)"
echo "DONE: $here/$APP"
