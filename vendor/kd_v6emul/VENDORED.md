# Vendored: kd_v6emul

Source: `best-effort-labs/kinduino` `arduino/cores/kindle/kd_v6emul.c` @ kinduino commit `5874bcb`.

License: **MIT** — kinduino contributors (governed by kinduino's root `LICENSE`).

What: a SIGILL-trap shim emulating the ARMv6K sub-word exclusives (LDREXB/STREXB/…),
CLREX, and DMB/DSB/ISB barriers that zig/LLVM emit for `-mcpu=arm1136jf_s` but that are
UNDEFINED on the i.MX ARM1136 r0 silicon. Inert on the K3 (its kernel handles them);
linked with `-DKD_V6EMUL_FORCE`. Single self-contained C file, no headers.

No local modifications.
