# Vendored: BearSSL

Source: https://bearssl.org/ (Thomas Pornin), as carried in
`best-effort-labs/kinduino` `arduino/libraries/BearSSL/` @ kinduino commit `5874bcb`.

License: **MIT** — © 2016 Thomas Pornin. Full text in `LICENSE.txt`.

Contents:
- `inc/` — public headers (used by `src/rsa_oaep.c`, `src/apple_response.c`, `src/airport_key.c`).
- `src/` — BearSSL source. The **host** static lib `libbearssl.a` is built from this by
  the Makefile (`make check`), so no host binary is committed (it would be arch-specific).
- `precompiled/arm1136jf_s/libBearSSL.a` — a precompiled BearSSL build for the K3's
  ARM1136 (soft-float ARMv6), committed as a binary because the device build links it
  directly. This mirrors upstream embedded practice and kinduino's own layout.

No local modifications.
