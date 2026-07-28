# Building shairkindle

Developer notes for building the daemon and the signed Kindlet from source. End users
don't need any of this — they can install the prebuilt `ShairKindle.azw2` from a
[release](../../../releases/latest) (see the main [README](../README.md)).

## Turnkey: Docker

One image builds the ARM daemon **and** the signed app:

```sh
docker compose -f docker/docker-compose.yml run --rm build
#   -> out-armv6/raopd + out-armv6/airplay-supervisor + kindlet/out/ShairKindle.azw2
docker compose -f docker/docker-compose.yml run --rm test
#   -> host unit suite (make clean && make check)
```

The `build` service cross-compiles a static soft-float ARMv6 `raopd` + `airplay-supervisor`
(zig / musl) and signs `ShairKindle.azw2` with the committed public keystore. The `test`
service is what CI runs, so local == CI.

## Native (host toolchain + zig, no Docker)

```sh
make check             # host unit tests (needs a C toolchain)
make raopd-armv6       # static ARMv6 raopd for the K3
make supervisor-armv6  # the lease supervisor
```

Building the Kindlet natively needs a **JDK-8 host** (`ecj` for Java-1.4 bytecode); see
[`kindlet/README.md`](../kindlet/README.md). The Docker path bundles this, so most people
should just use `docker compose ... build`.

### Toolchain gotcha (ARM1136 / SIGILL)

The K3's ARM1136 is ARMv6, **not** ARMv6T2. zig's `arm1136jf_s` CPU model wrongly enables
v6t2, so LLVM can emit `MOVW`/`MOVT` — undefined on ARM1136, and they `SIGILL` on the
device. The build uses `-mcpu=arm1136jf_s-v6t2` (the `-v6t2` suffix *disables* it → literal
pools instead) and links the `kd_v6emul` shim. Don't change those flags without testing on
real hardware. `clock_gettime` / `pthread_cond_timedwait` are also banned in `src/` (they
`SIGSEGV` on the K3's pre-vDSO musl); all bounded waits use `nanosleep` + `WNOHANG`.

## Signing & device generations

`kindlet/developer.keystore` is committed on purpose: it is the **public** jailbreak
developer keystore (aliases `ditest`/`dktest`/`dntest`, password `password`). It provides
package *identity*, not publisher *authenticity*, and its private keys are already public —
it is **not a secret**.

The K3 (firmware 3.4.x) signs **RSA / SHA-256**; the older K2 used **DSA / SHA1**. To build
for another generation, mount that generation's keystore over the default:

```sh
docker compose -f docker/docker-compose.yml run --rm -v /path/to.keystore:/keystore build
```

## Project layout

- `src/` — the native RAOP daemon (`raopd`) and lease supervisor. RTSP/RTP/SDP/DAAP/ALAC/
  mDNS/RSA/AES/DACP protocol code; parses untrusted LAN input (hence the defensive bounds
  checks and malformed-input tests).
- `kindlet/src/com/besteffortlabs/` — the Kindlet: lifecycle shell, self-installer, input
  routing, and the now-playing app.
- `kindlet/src/ixtab/` — the attributed third-party jailbreak frontend (used as a runtime
  API — see [`kindlet/src/ixtab/README.md`](../kindlet/src/ixtab/README.md)).
- `extension/` — on-device shell scripts, config, display assets, and the bundled FBInk CLI.
- `vendor/` — pinned third-party source, each with a `VENDORED.md` provenance file.
- `tests/` — host unit, lifecycle, parser, and shell tests.
- `docker/` — the reproducible build + test image.

## Display: bundled fbink & resolution order

The now-playing card is drawn by **FBInk** (NiLuJe, GPLv3-or-later). shairkindle bundles
its CLI as `extension/bin/fbink` and runs it as a **separate program** — it does not link
`libfbink`, which keeps shairkindle's own code MIT while the bundled binary stays GPLv3.
Corresponding source is offered per GPLv3 §6 in [`SOURCE-OFFER.txt`](../SOURCE-OFFER.txt).

At runtime `extension/bin/airplay-nowplaying` resolves fbink in this order: `$FBINK` (if
set) → a usbnet-installed fbink → the bundled `$here/fbink`. So a newer, already-installed
fbink is preferred; the bundle is the usbnet-free fallback.

## The install / payload model

The signed Kindlet embeds its entire on-device footprint as a **manifest-checked payload**
and, on first launch, installs it under `/var/local/shairkindle/`. Each file is content-hashed
against the manifest, written atomically, and the install is fail-closed (a partial write
never marks the version stamp as up-to-date). The manifest `version` is a hash of all file
contents, so any change to the payload forces a re-install on the next launch; files a newer
version no longer ships are pruned. See `kindlet/src/.../PayloadInstaller.java`.
