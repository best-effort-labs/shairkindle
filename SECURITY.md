# Security & trust model

shairkindle is **experimental software for jailbroken Kindles, provided as-is with no
warranty.** Unlike a typical app it runs a **network daemon** — read this first.

## Attack surface: raopd is network-facing

`raopd` listens on the LAN and parses **attacker-controlled input** — RTSP requests,
RTP audio packets, and ALAC frames from any AirPlay sender that can reach the device.
This is a real remote attack surface (the vendored ALAC decoder already carries a
documented heap-overflow clamp — see `vendor/alac/VENDORED.md`). Run shairkindle only on
networks you trust, and treat the daemon as exposed.

Like the rest of a jailbroken-Kindle toolchain, the binaries are **native ARM code you
compile and install yourself** — trusted code, not sandboxed. Only run builds you
produced or have read.

## Jailbreak and permission boundary

shairkindle requires the device owner to have installed the ixtab jailbreak/KUAL
environment beforehand. It does not jailbreak a stock Kindle.

At launch, the Kindlet calls ixtab's existing gateway. The gateway may enable an
in-memory Java policy wrapper, and shairkindle requests `AllPermission` for its Kindlet
protection domain so it can write `/var/local/shairkindle`, execute the bundled native
programs, and manage those processes. The application fails closed if that permission
check does not succeed.

This flow does **not** edit Amazon's `external.policy`, replace the on-disk developer
keystore, or persist a new system policy file. It is nevertheless privileged runtime
code. When the framework destroys the Kindlet, shairkindle disables the in-memory policy
wrapper if shairkindle was the code that enabled it. If the wrapper was already active,
shairkindle does not claim ownership and does not disable another Kindlet's shared
runtime state. Restarting the framework or device reconstructs policy state from disk;
shairkindle has not altered that disk policy.

The persistent application footprint is:

- `/mnt/us/documents/ShairKindle.azw2`, placed there by the device owner; and
- `/var/local/shairkindle`, populated from the signed Kindlet's manifest-checked payload.

`extension/bin/setup` is a no-op compatibility stub. No one-time setup or
`external.policy` modification is required.

## Firewall: enabling AirPlay opens ALL UDP on wlan0

When AirPlay is enabled, shairkindle installs a **blanket `iptables` rule accepting all
inbound UDP on the Wi-Fi interface** (`airplay_fw_open` in `extension/bin/airplay-lib.sh`),
not just its own RTP/control/timing ports. It does this because raopd lets the kernel pick
those ports ephemerally per session, so there is no fixed range to scope the rule to. The
effect: **while AirPlay is on, any UDP service listening on the device is reachable from
the LAN**, on an old, privileged, unsandboxed kernel. The rule is scoped to `wlan0` and is
removed again by `airplay_fw_close` when AirPlay is disabled or the app exits — so the
exposure lasts only while the receiver is running — but it is broader than shairkindle's
own sockets. Run it only on a trusted network. (Narrowing this to a fixed RTP port range
would require raopd to bind its RTP/control/timing sockets in a configured range; tracked
as a future improvement.)

## Reporting an issue

For a **security issue** — a crash or memory-safety bug reachable from the network —
please email **besteffortlabs@heliodox.com** rather than opening a public issue, so a
fix can land before the details are public. Include your model, firmware, exact steps,
and logs. For non-sensitive bugs (including a reproducible device-bricking bug), a public
issue with the same detail is fine. This is a best-effort community project with no
formal disclosure SLA.

---

*Not affiliated with, endorsed by, or sponsored by Apple or Amazon. "AirPlay" and
"AirPort Express" are trademarks of Apple Inc.; "Kindle" identifies the target devices.*
