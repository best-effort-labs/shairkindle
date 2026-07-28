# shairkindle shared helpers -- sourced by both the installed scripts and the
# optional KUAL extension wrappers. The runtime bundle is flat under PREFIX.

CONFIG="${AIRPLAY_CONFIG:-${AIRPLAY_PREFIX:-/var/local/shairkindle}/config}"
# Debug logging is OFF by default: raopd's per-second stream stats otherwise grow
# airplay.log unbounded and fill the tiny (~23 MB) /var/local partition over time.
# Opt in by creating this sentinel -- reachable BOTH ways a Kindle owner has: drop
# an empty file on the USB drive (/mnt/us mounts as mass storage), or `touch` it
# over ssh. Then relaunch. Overridable for host tests.
DEBUG_FLAG="${AIRPLAY_DEBUG_FLAG:-/mnt/us/shairkindle-debug}"
IPT=/usr/sbin/iptables                 # iptables is NOT in $PATH on stock Kindle
[ -x "$IPT" ] || IPT=$(command -v iptables 2>/dev/null || true)   # || true: survive set -e
WLAN="${AIRPLAY_WLAN:-wlan0}"

# Materialize the documented defaults on first receiver launch so the optional
# KUAL menu can inspect and toggle them. Never overwrite a user's config.
airplay_config_ensure() {
    [ -f "$CONFIG" ] && return 0
    mkdir -p "$(dirname "$CONFIG")" || return 1
    {
        echo "WIFI=true"
        echo "AIRPLAY_NAME=ShairKindle"
    } > "$CONFIG.tmp" || return 1
    mv "$CONFIG.tmp" "$CONFIG"
}

# echo "true" iff AirPlay-over-WiFi is enabled. A missing config defaults true:
# the self-contained Kindlet install is explicitly usbnet-free. Users can persist
# WIFI=false with airplay-wifi for USB-only operation.
airplay_wifi_on() {
    [ -f "$CONFIG" ] || { echo true; return; }
    v=$(sed -n 's/^WIFI=\(.*\)$/\1/p' "$CONFIG" | tail -1)
    [ "$v" = "false" ] && echo false || echo true
}

# echo "true" iff debug logging is enabled (sentinel present). Default false so a
# clean device never accumulates logs; users opt in per DEBUG_FLAG above.
airplay_debug_on() { [ -f "$DEBUG_FLAG" ] && echo true || echo false; }

# echo the configured AirPlay name (default "ShairKindle" when absent/empty).
# Trailing whitespace trimmed; the C-side raop_name_sanitize does the strict
# (mDNS byte-budget / control-char) cleanup — this is just the config read.
airplay_name() {
    [ -f "$CONFIG" ] || { echo ShairKindle; return; }
    v=$(sed -n 's/^AIRPLAY_NAME=\(.*\)$/\1/p' "$CONFIG" | tail -1)
    v=${v%"${v##*[![:space:]]}"}          # strip trailing whitespace
    [ -n "$v" ] && echo "$v" || echo "ShairKindle"
}

# remove our wlan0 ACCEPT rules (idempotent; no-op when there's no iptables)
airplay_fw_close() {
    [ -n "$IPT" ] || return 0
    for spec in "-p tcp --dport 5000" "-p udp" "-p icmp"; do
        while "$IPT" -D INPUT -i "$WLAN" $spec -j ACCEPT 2>/dev/null; do :; done
    done
}

# Open wlan0 for AirPlay: RTSP tcp/5000 + all UDP + ICMP. UDP is currently
# -p udp because the RTP server_port is chosen per-session (and mDNS is 5353) --
# narrow to the RTP range if raopd ever exposes it. Idempotent (close first);
# no-op without iptables so host `make check` stays green.
airplay_fw_open() {
    [ -n "$IPT" ] || return 0
    airplay_fw_close
    "$IPT" -I INPUT -i "$WLAN" -p tcp --dport 5000 -j ACCEPT
    "$IPT" -I INPUT -i "$WLAN" -p udp -j ACCEPT
    "$IPT" -I INPUT -i "$WLAN" -p icmp -j ACCEPT
}

# --- AR6000 WiFi power-save ---------------------------------------------------
# `iwconfig wlan0 power off` does NOT actually disable power-save on this chip --
# an idle radio buffers inbound unicast (measured 80-300ms latency, avg 82ms),
# which breaks real-time RTP audio and connection handshakes. wmiconfig maxperf
# truly disables it (measured avg 11ms / max 38ms after). Set on WiFi-enable,
# restored to the recommended profile on disable to spare the battery.
WMICONFIG="${AIRPLAY_WMICONFIG:-/sbin/wmiconfig}"   # seam: tests point at a capturing fake
airplay_wifi_maxperf()   { [ -x "$WMICONFIG" ] && "$WMICONFIG" -i "$WLAN" --power maxperf >/dev/null 2>&1; return 0; }
airplay_wifi_powersave() { [ -x "$WMICONFIG" ] && "$WMICONFIG" -i "$WLAN" --power rec     >/dev/null 2>&1; return 0; }

# Exit 0 iff /proc/<pid>/exe resolves to a binary named "raopd". $AIRPLAY_PROC
# overrides /proc for host tests. Guards airplay-off against PID reuse: the
# pidfile PID may have been recycled to an unrelated process.
pid_looks_like_raopd() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac      # positive decimal only
    _pr="${AIRPLAY_PROC:-/proc}"
    _exe=$(readlink "$_pr/$1/exe" 2>/dev/null) || return 1
    [ -n "$_exe" ] || return 1
    case "$_exe" in
        */raopd|raopd|*"/raopd (deleted)"|"raopd (deleted)") return 0 ;;
        *)                                                    return 1 ;;
    esac
}

# Exit 0 iff pid $1 is a VALID airplay-supervisor group leader: /proc/<pid>/exe
# names airplay-supervisor, PID==PGID==SID (session leader), and starttime (field
# 22) matches the pidfile token $2. Root kills the whole GROUP off this pid, so a
# stale pidfile pointing at a recycled/unrelated pid must NEVER validate.
# $AIRPLAY_PROC overrides /proc for host tests.
# Exit 0 iff pid $1 is an airplay-supervisor SESSION/GROUP LEADER: /proc/<pid>/exe
# names airplay-supervisor AND PID==PGID==SID. Sets $_start (its starttime) as a
# side effect. This is the token-LESS check used by the ps fallback (no pidfile
# token to compare); supervisor_pid_valid adds the starttime match on top.
supervisor_leader_ok() {
    _pid="$1"; _pr="${AIRPLAY_PROC:-/proc}"; _start=""
    case "$_pid" in ''|*[!0-9]*) return 1 ;; esac
    _exe=$(readlink "$_pr/$_pid/exe" 2>/dev/null) || return 1
    case "$_exe" in
        */airplay-supervisor|airplay-supervisor|*"/airplay-supervisor (deleted)") : ;;
        *) return 1 ;;
    esac
    _stat=$(cat "$_pr/$_pid/stat" 2>/dev/null) || return 1
    # tokens AFTER the last ')': state=1 ppid=2 pgrp=3 session=4 ... starttime=20
    eval "$(printf '%s\n' "$_stat" | awk '{
        s=$0; sub(/.*\) /, "", s); n=split(s, a, " ");
        printf "_pgrp=%s; _sess=%s; _start=%s\n", a[3], a[4], a[20] }')"
    [ "$_pgrp" = "$_pid" ] && [ "$_sess" = "$_pid" ]
}

# Exit 0 iff pid $1 is a valid supervisor leader AND its starttime matches the
# pidfile token $2 (PID-reuse guard). Root kills the whole GROUP off this pid, so
# a stale pidfile pointing at a recycled/unrelated pid must NEVER validate.
supervisor_pid_valid() {
    supervisor_leader_ok "$1" || return 1
    [ "$_start" = "$2" ]
}
