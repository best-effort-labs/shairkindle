# shairkindle now-playing shell kindlet

A minimal foreground **Kindlet** (`.azw2`) whose only job is to hold input focus
so the reader framework stops self-navigating on button presses, while `raopd`
paints the now-playing card into the same e-ink framebuffer via `fbink`. It draws
nothing itself; it instruments paint/key callbacks (visible in the title bar).

Validated on a real K3 running firmware 3.4.3: the signed Kindlet launches,
holds focus (5-way/page/menu all reach it), and FBInk content coexists with its
blank surface without being clobbered.

## Building requires a JDK-8 host (this Mac has no Java runtime)

`build-sign.sh` runs on a JDK-8 host (Java 8 plus `ecj` for Java-1.4 bytecode). The
project's `docker/` image provides exactly this, so `docker compose ... build` is the
easy path. It is **self-contained**: it compiles
against the clean-room API stubs in `stubs/` and defaults to the committed
public jailbreak developer keystore:

```sh
# on the JDK-8 host, from this kindlet/ dir:
sh build-sign.sh
# -> out/ShairKindle.azw2
```

Deploy + launch on the K3:

```sh
cat out/ShairKindle.azw2 | ssh <k3> 'cat > /mnt/us/documents/ShairKindle.azw2'
# then restart the framework once so the Home scanner indexes it:
ssh <k3> '/etc/init.d/framework restart'      # ~30s; native raopd survives it
# tap "ShairKindle" on Home. Success in /var/log/messages:
#   implementationId=test   (failure: implementationId=null + "not signed by a registered developer")
```

No separate setup command or `external.policy` edit is required. The target must already
have the ixtab jailbreak/KUAL environment installed by its owner. On launch, the Kindlet
uses that existing runtime gateway, requests `AllPermission` for its own protection
domain, installs the bundled payload under `/var/local/shairkindle`, and starts the
supervisor.

The ixtab frontend may enable an in-memory policy wrapper in the running Java framework.
shairkindle does not write that wrapper to a policy file and does not modify
`external.policy`. On Kindlet destruction it disables the wrapper if it was responsible
for enabling it; a wrapper that was already active remains owned by the other Kindlet.
See the repository README's “What the jailbreak gateway changes” section and
`SECURITY.md` for the complete trust boundary.

## Signing — the K3 uses RSA/SHA-256, NOT DSA/SHA1 (device-specific!)

**The single most important fact.** Kindle dev-cert parameters differ by firmware/device
generation:

| Device | Key algo | Signature | Digest | Block |
|--------|----------|-----------|--------|-------|
| K2 (older prior art) | DSA-1024 | SHA1withDSA | SHA1 (no-hyphen `SHA1-Digest-Manifest`) | `.DSA` |
| **K3 (this device, fw 3.4.3)** | **RSA-2048** | **SHA256withRSA** | **SHA-256** (`SHA-256-Digest-Manifest`) | **`.RSA`** |

The committed `developer.keystore` is the shared public jailbreak keystore used by
KUAL-era Kindlets. Its aliases and private keys are already publicly distributed; it
provides a package identity accepted by devices with the matching developer certificates,
not private publisher authentication. Signing the K3 with the older K2 DSA scheme fails
because the signature algorithm and installed certificates do not match.

**How to confirm or override the scheme for another Kindle generation:**

1. Inspect the committed default:
   `keytool -list -keystore developer.keystore -storepass password`.
2. Read the algorithm from its certs (`keytool -list -v … | grep "Public Key Algorithm"`)
   AND from a **working installed** app's signature (definitive):
   `unzip -l /mnt/us/documents/KUAL*.azw2` → `.RSA` vs `.DSA` block;
   `unzip -p … META-INF/DITEST.SF | head` → `SHA-256-` vs `SHA1-Digest-Manifest`.
3. If the target uses a different installed developer certificate set, pass a compatible
   keystore with `KEYSTORE=/path/to/compatible.keystore`. Set `SIGALG`/`DIGESTALG` to
   match; defaults are the K3's `SHA256withRSA` / `SHA-256`.

`build-sign.sh` verifies the resulting JAR signature, but the **real** compatibility gate
is a launch on the target generation.

## Bytecode: major-48 (Java 1.4)

The K3 cvm loads major-48 bytecode (both Amazon's own `Kindlet-1.2.jar` and KUAL
are 1.4). JDK-8 `javac` can't emit below 1.5, so we compile with **`ecj`**
(`-source 1.4 -target 1.4`). Source is kept 1.4-compatible (no generics/
autoboxing/enhanced-for).

## Clean-room API stubs (why there's no Amazon jar here)

`stubs/com/amazon/kindle/kindlet/*.java` are **our own** minimal declarations of
the few Kindlet API members this kindlet calls (`AbstractKindlet`, `Kindlet`,
`KindletContext.{getRootContainer,getHomeDirectory,setSubTitle}`) — empty bodies,
signatures verified against the on-device `Kindlet-1.2.jar` via `javap`. We
compile against them and **exclude them from the `.azw2`**; the Kindle framework
supplies the real classes at runtime (same FQCNs → binary-compatible). So we ship
no proprietary Amazon bytecode and nobody needs to extract a jar. Building
against the real jar instead is possible but unnecessary: `KINDLET_JAR=... sh build-sign.sh`.

## Public build inputs and files kept out

- **`developer.keystore` is intentionally committed.** It is the public shared jailbreak
  keystore described above, not a project secret.

`.gitignore` excludes generated or proprietary local inputs:

- **`out/`**, `classes/`, `stub-classes/` — build output.
- **`Kindlet-1.2.jar`** — Amazon's proprietary API jar. Not needed (stubs replace
  it); excluded so it never lands in the repo even if someone pulls it locally.

## Instrumentation

Without the already-installed jailbreak gateway, the Kindlet sandbox blocks the
filesystem and process operations this application needs. Before the runtime permission
step succeeds, diagnostics therefore use the title bar via `setSubTitle`:
`keys=<n> <lastkeyname> p=<paints>`. Read them from a framebuffer capture
(`dd if=/dev/fb0`, 600×800 4bpp stride-300, high-nibble-left, inverted → unpack).
The 5-way/page/menu key codes are named from
`com.amazon.kindle.kindlet.event.KindleKeyCodes`.
