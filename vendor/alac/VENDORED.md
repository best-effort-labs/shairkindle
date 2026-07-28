# Vendored: hairtunes ALAC decoder

Source: https://github.com/abrasive/shairport (files `alac.c`, `alac.h`),
repo commit `c627bae`. Original author David Hammerton
(http://crazney.net/programs/itunes/alac.html).

License: **MIT** — the full MIT text is the header comment in `alac.c`
(Copyright (c) 2005 David Hammerton, all rights reserved).

Files: `alac.c`, `alac.h`. Copied verbatim except for the local security patch
below; do not re-fetch (a different revision could break the pre-validated
`tests/fixture_alac.c` round-trip).

## Local modifications

Our copy carries a local security patch clamping the attacker-controlled
`outputsamples` to `setinfo_max_samples_per_frame` in `alac_decode_frame` (BOTH
the mono and stereo channel branches, at each `outputsamples = readbits(alac, 32)`
inside the `if (hassize)` block). Upstream reads a 32-bit frame size with no
bounds check and uses it to size decode loops writing into fixed-size heap
buffers — a single crafted frame yields an out-of-bounds heap write. The patch
sets `*outputsize = 0` and returns early on an oversized value (grep for
`shairkindle local security patch`). A valid ALAC frame never exceeds
`setinfo_max_samples_per_frame`, so this is lossless-safe. **Must be reapplied
if `alac.c` is ever updated from upstream.**

## API used by the shim (`src/alac_shim.c`)

- `alac_file *alac_create(int samplesize, int numchannels);`
- `void alac_set_info(alac_file *alac, char *inputbuffer);` — parses the 48-byte
  QuickTime `frma`/`alac` magic cookie; internally calls `alac_allocate_buffers`.
- `void alac_decode_frame(alac_file *alac, unsigned char *inbuffer, void *outbuffer, int *outputsize);`
  — `outputsize` is in BYTES.
- `void alac_free(alac_file *alac);`

## Build note

`alac.c` emits warnings under `-Wall -Wextra`, so the Makefile compiles ONLY
`vendor/alac/alac.c` with `-w` (warnings suppressed) — our own code keeps the
full warning set. See the `vendor/alac/alac.o` rule.
