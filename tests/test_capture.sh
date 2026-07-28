#!/bin/sh
# Host integration test for the now-playing body-capture path (Task B3).
# Launches `raopd --smoke <port>` with AIRPLAY_PREFIX=/tmp/npcap, then drives a
# >RBUF_CAP `Content-Type: image/jpeg` SET_PARAMETER through the OVERSIZED
# streaming path and asserts:
#   (1) /tmp/npcap/np-art.jpg is written with the EXACT body bytes, and
#   (2) a request pipelined AFTER the giant body is still parsed (framing
#       survived the capture — the bug this whole feature had to avoid).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

command -v python3 >/dev/null 2>&1 || { echo "test_capture SKIP (no python3)"; exit 0; }

raopd="$root/tests/smoke-raopd"
port=45231
prefix=/tmp/npcap
art="$prefix/np-art.jpg"
rm -rf "$prefix"; mkdir -p "$prefix"

# Fake renderer (Task NP-3): proves the real daemon wires art-capture ->
# np_publish_art -> nprender_wake -> worker -> execv end-to-end. It just
# appends its mode arg ($1) to a log file; it doesn't read/render anything.
fake_bin="$prefix/fake-nprender.sh"
fake_log="$prefix/fake-nprender.log"
cat > "$fake_bin" <<SH
#!/bin/sh
echo "\$1" >> "$fake_log"
SH
chmod +x "$fake_bin"

cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

AIRPLAY_PREFIX="$prefix" AIRPLAY_NOWPLAYING_BIN="$fake_bin" "$raopd" --smoke "$port" &
pid=$!

AIRPLAY_ART="$art" AIRPLAY_PORT="$port" python3 - "$@" <<'PY'
import os, socket, sys, time
port = int(os.environ["AIRPLAY_PORT"]); art = os.environ["AIRPLAY_ART"]

# known body, comfortably > RBUF_CAP (8192) to force the oversized path
body = bytes((i * 37 + 11) & 0xff for i in range(20000))

# connect (bounded retry until the listener is up)
s = None
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2); break
    except OSError:
        time.sleep(0.1)
if s is None:
    print("FAIL: could not connect"); sys.exit(1)

req = (b"SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\n"
       b"CSeq: 5\r\nContent-Type: image/jpeg\r\n"
       b"Content-Length: %d\r\n\r\n" % len(body)) + body
# a pipelined OPTIONS right after the giant body proves framing survived
tail = b"OPTIONS * RTSP/1.0\r\nCSeq: 6\r\n\r\n"

s.sendall(req + tail)
s.settimeout(3)
resp = b""
try:
    while b"CSeq: 6" not in resp:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
except socket.timeout:
    pass
s.close()

if b"CSeq: 6" not in resp:
    print("FAIL: pipelined request after body not answered (framing lost)"); sys.exit(1)

# give the daemon a beat to fsync+rename, then check the captured bytes
for _ in range(50):
    if os.path.exists(art): break
    time.sleep(0.1)
if not os.path.exists(art):
    print("FAIL: art file not written"); sys.exit(1)
with open(art, "rb") as f:
    got = f.read()
if got != body:
    print("FAIL: art bytes mismatch (len %d vs %d)" % (len(got), len(body))); sys.exit(1)
print("test_capture OK")
PY

# Bounded poll (no fixed sleep) for the fake renderer to have run. --smoke
# never reaches RECORD so `playing` stays 0 -> the worker draws mode "splash"
# (not the metadata draw path — that's test_nprender's job). This only proves
# the wake wiring reaches the worker inside the real daemon.
i=0
while [ ! -s "$fake_log" ] && [ "$i" -lt 30 ]; do
    sleep 0.1
    i=$((i + 1))
done
if [ ! -s "$fake_log" ]; then
    echo "FAIL: fake renderer never ran (wake wiring did not reach worker)"; exit 1
fi
if ! grep -q splash "$fake_log"; then
    echo "FAIL: fake renderer ran but not with mode 'splash' (got: $(cat "$fake_log"))"; exit 1
fi
echo "test_capture wiring OK (fake renderer invoked: $(cat "$fake_log" | tr '\n' ' '))"

# Region-clear guard (partial-refresh): the new-track flash must clear only the
# card region, NOT the whole screen, so it can't blank the framework's top chrome
# strip. The region suboptions MUST be ATTACHED to -k in one token (-k"<region>"):
# a bare `-k` with the region as a separate token clears the WHOLE screen (HW-
# confirmed on the K3). The region is factored into $CARD_REGION (top=...).
nowplaying="$root/extension/bin/airplay-nowplaying"
if grep -q -- '-c -f' "$nowplaying"; then
    echo "FAIL: airplay-nowplaying still uses full-screen clear (-c -f) on flash"; exit 1
fi
if ! grep -q '^CARD_REGION="top=' "$nowplaying"; then
    echo "FAIL: \$CARD_REGION must be an attached region spec starting 'top='"; exit 1
fi
if ! grep -qF -- '-k"$CARD_REGION"' "$nowplaying"; then
    echo "FAIL: flash must clear via the ATTACHED region -k\"\$CARD_REGION\", not a bare -k"; exit 1
fi
# De-ghost (hardware-validated): legacy einkfb ignores fbink -f, so the flash
# forces a real pixel swing by painting the band BLACK then WHITE.
if ! grep -q -- '-B BLACK -k"$CARD_REGION"' "$nowplaying" || ! grep -q -- '-B WHITE -k"$CARD_REGION"' "$nowplaying"; then
    echo "FAIL: flash_band must de-ghost via BLACK then WHITE region clears"; exit 1
fi
echo "test_capture region-clear guard OK"

# Splash mode must: region-flash the card band (attached -k), paint the splash
# PNG, and overlay the configured name as a --guarded text line.
if ! grep -qi 'file=.*splash' "$nowplaying"; then
    echo "FAIL: splash mode must fbink -g the splash image"; exit 1
fi
if ! grep -q 'AIRPLAY_NAME:-' "$nowplaying"; then
    echo "FAIL: splash mode must render the configured \$AIRPLAY_NAME"; exit 1
fi
# Card art fallback: when no art, draw the placeholder instead of nothing.
if ! grep -q 'art-placeholder' "$nowplaying"; then
    echo "FAIL: card path must fall back to art-placeholder when art is absent"; exit 1
fi
# The retired 'clear' mode must be gone.
if grep -q 'clear)' "$nowplaying"; then
    echo "FAIL: 'clear' mode should be retired (now-playing draws splash when !playing)"; exit 1
fi
echo "test_capture splash/placeholder guards OK"
