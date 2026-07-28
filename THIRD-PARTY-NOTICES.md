# Third-Party Notices

shairkindle (MIT © 2026 Tymm Zerr) incorporates and/or links the following. The
`raopd` device binary is **statically linked** (zig/musl toolchain), so it embeds
several of these at distribution time — MIT ("...in all copies or substantial
portions...") and BSD-3-Clause (clause 2, "...reproduce ... in the documentation or
other materials...") both require that their notices travel with the binary. This file
discharges that.

## Kindlet runtime frontend

### ixtab kindlejailbreak frontend — `kindlet/src/ixtab/` (WTFPL)

The attributed Java frontend is included so the Kindlet can call the ixtab gateway
already installed by the device owner. It is a runtime compatibility API, not a
jailbreak installer. Upstream source release and local provenance notes are recorded in
`kindlet/src/ixtab/README.md`. The source files state ixtab's WTFPL grant.

## Vendored in-tree (see each `vendor/<name>/VENDORED.md` for provenance + pinned SHA)

### ALAC decoder — `vendor/alac/`  (MIT © 2005 David Hammerton, via shairport)

```
Copyright (c) 2005 David Hammerton
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be included in all copies
or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

> Our copy carries a local security patch (heap-overflow clamp in `alac_decode_frame`);
> see `vendor/alac/VENDORED.md`. The MIT grant above permits modification.

### tinysvcmdns — `vendor/tinysvcmdns/`  (BSD-3-Clause © 2011 Darell Tan)

```
Copyright (c) 2011, Darell Tan
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list
   of conditions and the following disclaimer in the documentation and/or other
   materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### BearSSL — `vendor/bearssl/`  (MIT © 2016 Thomas Pornin)

```
Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be included in all copies
or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### kd_v6emul — `vendor/kd_v6emul/`  (MIT © 2026 kinduino contributors)

A single self-contained C file (ARMv6K exclusives/barriers SIGILL-trap shim); see
`vendor/kd_v6emul/VENDORED.md`. Governed by the `best-effort-labs/kinduino` root
`LICENSE`, reproduced verbatim:

```
MIT License

Copyright (c) 2026 kinduino contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Embedded in the distributed `raopd` binary via static linking (zig toolchain)

### musl libc  (MIT © 2005-2020 Rich Felker, et al.)

Supplied by the zig 0.13.0 toolchain (`lib/libc/musl/`) and statically linked into
`raopd`. Full upstream `COPYRIGHT`, verbatim:

```
musl as a whole is licensed under the following standard MIT license:

----------------------------------------------------------------------
Copyright © 2005-2020 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
----------------------------------------------------------------------

Authors/contributors include:

A. Wilcox
Ada Worcester
Alex Dowad
Alex Suykov
Alexander Monakov
Andre McCurdy
Andrew Kelley
Anthony G. Basile
Aric Belsito
Arvid Picciani
Bartosz Brachaczek
Benjamin Peterson
Bobby Bingham
Boris Brezillon
Brent Cook
Chris Spiegel
Clément Vasseur
Daniel Micay
Daniel Sabogal
Daurnimator
David Carlier
David Edelsohn
Denys Vlasenko
Dmitry Ivanov
Dmitry V. Levin
Drew DeVault
Emil Renner Berthing
Fangrui Song
Felix Fietkau
Felix Janda
Gianluca Anzolin
Hauke Mehrtens
He X
Hiltjo Posthuma
Isaac Dunham
Jaydeep Patil
Jens Gustedt
Jeremy Huntwork
Jo-Philipp Wich
Joakim Sindholt
John Spencer
Julien Ramseier
Justin Cormack
Kaarle Ritvanen
Khem Raj
Kylie McClain
Leah Neukirchen
Luca Barbato
Luka Perkov
M Farkas-Dyck (Strake)
Mahesh Bodapati
Markus Wichmann
Masanori Ogino
Michael Clark
Michael Forney
Mikhail Kremnyov
Natanael Copa
Nicholas J. Kain
orc
Pascal Cuoq
Patrick Oppenlander
Petr Hosek
Petr Skocik
Pierre Carrier
Reini Urban
Rich Felker
Richard Pennington
Ryan Fairfax
Samuel Holland
Segev Finer
Shiz
sin
Solar Designer
Stefan Kristiansson
Stefan O'Rear
Szabolcs Nagy
Timo Teräs
Trutz Behn
Valentin Ochs
Will Dietz
William Haddon
William Pitcock

Portions of this software are derived from third-party works licensed
under terms compatible with the above MIT license:

The TRE regular expression implementation (src/regex/reg* and
src/regex/tre*) is Copyright © 2001-2008 Ville Laurikari and licensed
under a 2-clause BSD license (license text in the source files). The
included version has been heavily modified by Rich Felker in 2012, in
the interests of size, simplicity, and namespace cleanliness.

Much of the math library code (src/math/* and src/complex/*) is
Copyright © 1993,2004 Sun Microsystems or
Copyright © 2003-2011 David Schultz or
Copyright © 2003-2009 Steven G. Kargl or
Copyright © 2003-2009 Bruce D. Evans or
Copyright © 2008 Stephen L. Moshier or
Copyright © 2017-2018 Arm Limited
and labelled as such in comments in the individual source files. All
have been licensed under extremely permissive terms.

The ARM memcpy code (src/string/arm/memcpy.S) is Copyright © 2008
The Android Open Source Project and is licensed under a two-clause BSD
license. It was taken from Bionic libc, used on Android.

The AArch64 memcpy and memset code (src/string/aarch64/*) are
Copyright © 1999-2019, Arm Limited.

The implementation of DES for crypt (src/crypt/crypt_des.c) is
Copyright © 1994 David Burren. It is licensed under a BSD license.

The implementation of blowfish crypt (src/crypt/crypt_blowfish.c) was
originally written by Solar Designer and placed into the public
domain. The code also comes with a fallback permissive license for use
in jurisdictions that may not recognize the public domain.

The smoothsort implementation (src/stdlib/qsort.c) is Copyright © 2011
Valentin Ochs and is licensed under an MIT-style license.

The x86_64 port was written by Nicholas J. Kain and is licensed under
the standard MIT terms.

The mips and microblaze ports were originally written by Richard
Pennington for use in the ellcc project. The original code was adapted
by Rich Felker for build system and code conventions during upstream
integration. It is licensed under the standard MIT terms.

The mips64 port was contributed by Imagination Technologies and is
licensed under the standard MIT terms.

The powerpc port was also originally written by Richard Pennington,
and later supplemented and integrated by John Spencer. It is licensed
under the standard MIT terms.

All other files which have no copyright comments are original works
produced specifically for use as part of this library, written either
by Rich Felker, the main author of the library, or by one or more
contibutors listed above. Details on authorship of individual files
can be found in the git version control history of the project. The
omission of copyright and license comments in each file is in the
interest of source tree size.

In addition, permission is hereby granted for all public header files
(include/* and arch/*/bits/*) and crt files intended to be linked into
applications (crt/*, ldso/dlstart.c, and arch/*/crt_arch.h) to omit
the copyright notice and permission notice otherwise required by the
license, and to use these files without any requirement of
attribution. These files include substantial contributions from:

Bobby Bingham
John Spencer
Nicholas J. Kain
Rich Felker
Richard Pennington
Stefan Kristiansson
Szabolcs Nagy

all of whom have explicitly granted such permission.

This file previously contained text expressing a belief that most of
the files covered by the above exception were sufficiently trivial not
to be subject to copyright, resulting in confusion over whether it
negated the permissions granted in the license. In the spirit of
permissive licensing, and of not having licensing issues being an
obstacle to adoption, that text has been removed.
```

### zig-bundled runtime — compiler-rt / libc++  (Apache-2.0 WITH LLVM-exception; legacy parts MIT)

The statically-linked `raopd` embeds a small amount of LLVM-project runtime supplied by
the **zig 0.13.0** toolchain: compiler-rt builtins (soft-float ARMv6 helpers) and, for the
C++ translation units, libc++/libc++abi. These are licensed under the **Apache License 2.0
with the LLVM Exception** (with some legacy portions under MIT). The full applicable
license text — including the LLVM Exception and the legacy MIT notices — is reproduced
in-tree at [`licenses/LLVM-LICENSE.txt`](licenses/LLVM-LICENSE.txt) (verbatim from the zig
0.13.0 distribution's `lib/libcxx/LICENSE.TXT`). No modifications.

## Distributed GPLv3-or-later binary (see SOURCE-OFFER.txt)

### FBInk — `extension/bin/fbink`  (GPLv3-or-later, © NiLuJe and FBInk contributors)

shairkindle bundles NiLuJe's FBInk CLI as `extension/bin/fbink` (a statically-linked K3
ARMv6 build, FBInk v1.25.0-535-g83110d3d) so the now-playing display works without usbnet.
FBInk is distributed and executed as a **separate program**; shairkindle does not link
against `libfbink`. shairkindle's own original code stays MIT, while this bundled FBInk
executable remains **GPLv3-or-later**; its corresponding source is offered per GPLv3 §6 in
`SOURCE-OFFER.txt` (FBInk upstream at commit
`83110d3d278cf9cd44cc1d16237e284a89f72633`; also mirrored in `best-effort-labs/kinduino`).
This is the ONLY GPL component shairkindle distributes.

## Embedded well-known key

### AirPort Express RSA key — `src/airport_key.c`

The well-known AirPort Express private RSA key, public since ~2011 and reproduced by
shairport and its descendants ever since. RAOP's challenge-response handshake requires it,
so `raopd` embeds it. It is not a shairkindle secret; it is Apple's key, republished for
protocol compatibility.
