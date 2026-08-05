# Contributing, support & scope

Read this before opening an issue, to get a sense of expectations.

## What shairkindle is

A **best-effort hobby project**, not a maintained product. It was built to turn a jailbroken Kindle
Keyboard into an AirPlay speaker; it turned out well enough to be worth sharing, and it's meant to be
**usable and forkable** — but there is more work here than time to do it.

Concretely:

- **I'll fix reproducible bugs when I have time.**
- **I'm unlikely to provide one-on-one setup help.** Getting a specific K3 + jailbreak + sender
  working involves real device variance, much of it not specific to shairkindle.
- **Forks and PRs are welcome and encouraged.** Carry it forward without me if you like — that's a
  feature.

These little machines have plenty of life left in them.

## Community

shairkindle stands on years of Kindle-hacking and AirPlay reverse-engineering work, and the wider
community is the best place for help with the parts shairkindle doesn't cover — jailbreaking, KUAL,
usbnet, device-specific setup:

- **[Kindle Modding Community Discord](https://kindlemodding.org/)** — the active hub for current
  jailbreaks and modding.
- **[MobileRead forums & wiki](https://www.mobileread.com/)** — the long-form archive of
  Kindle-hacking knowledge (KUAL, usbnet, per-model pages).

## Scope

**In scope:** the `raopd` AirPlay-1 (RAOP) receiver daemon, the Kindlet (lifecycle, self-installer,
input, now-playing display), the DACP transport relay, and the docs.

**Out of scope:** other Kindle models — the ARMv6 build and the legacy e-ink/framebuffer handling are
tuned to the **K3** (a port to another model would be a genuinely interesting fork); **AirPlay 2**
(no buffered mode, multi-room, or video — this is AirPlay-1 audio only); and jailbreaking a stock
Kindle (shairkindle uses the jailbreak you've already installed).

**Known rough edges:** on rare occasions there's **no audio on the very first launch after a cold
reboot** (the Kindle's audio mixer needs a warm-up) — press Home and reopen. Root cause unconfirmed.

## Before you open an issue

**Check these first** — the two most common failures both imitate a shairkindle bug:

1. **Does KUAL itself open?** shairkindle is a developer-signed app like KUAL. If KUAL fails with
   *"not signed by a registered developer,"* your Kindle's developer certificates expired
   (2025-04-17) — install the refresh (see the README's *Developer certificates* section).
   shairkindle needs exactly the same trust, nothing extra.
2. **Are the Kindle and your sender on the same Wi-Fi network?** If "ShairKindle" doesn't appear in
   the AirPlay picker, this is almost always why.

For a **reproducible bug**, please include:

- **K3 firmware version** (Settings → Menu → Device Info; e.g. `3.4.3`)
- The **sender** you streamed from — iPhone/iPad/Mac and its OS version
- **`supervisor.log`** and **`airplay.log`** from `/var/local/shairkindle/`. Logging is off by
  default; enable it by creating an empty file `shairkindle-debug` at the root of the Kindle's USB
  drive (or `touch /mnt/us/shairkindle-debug` over ssh) and relaunching — see the README's *Debug
  logging* section.
- What you'd already tried

> **Redact before pasting logs:** `airplay.log` contains your Wi-Fi IP and the sender's IP/name.
> Nobody needs those to debug your problem.

Issues without this are hard to act on and will likely just sit.

## Sending a PR

- **Keep changes scoped.** One thing per PR.
- **Say how you tested it** — and be honest about what you *didn't* test. "Builds clean, host tests
  pass, not tried on hardware" is a useful and welcome PR; silently implying hardware validation is
  not.
- **Run the host tests + build** (no toolchain needed beyond Docker):
  ```sh
  docker compose -f docker/docker-compose.yml run --rm test
  docker compose -f docker/docker-compose.yml run --rm build
  ```
  See [`docs/BUILDING.md`](docs/BUILDING.md) for details.
- **Match the surrounding code.** It's plain C (daemon) and Java-1.4-era Kindlet code with dense
  comments that explain *why* — especially around the RAOP, concurrency, and teardown paths, where
  the comments encode something painful that was learned on hardware. Don't tidy those away.
- **Don't introduce `clock_gettime` / `pthread_cond_timedwait` in `src/`** — they SIGSEGV on the
  K3's pre-vDSO musl (see `docs/BUILDING.md`).

Design docs and worklogs are not published — they live in a private workspace. The reasoning that
matters is in the commit messages and the comments.

---

*Not affiliated with, endorsed by, or sponsored by Apple or Amazon. "AirPlay" and "AirPort Express"
are trademarks of Apple Inc.; "Kindle" identifies the target device.*
