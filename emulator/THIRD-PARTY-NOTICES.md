# Third-party notices

The Picocomputer 6502 emulator is distributed as two binary artifacts that
statically include third-party code:

- **desktop** — the native `rp6502-emu` executable (Linux / macOS / Windows),
  including the source-level debugger.
- **web** — the WebAssembly build (`rp6502.wasm` + `rp6502.js`) published to
  GitHub Pages.

A few components are **windows-only** (linked only in the Windows desktop build).
The web build excludes the debugger, so it does **not** include Dear ImGui or
cppdap. No game ROMs are bundled in either artifact.

**Every bundled component is under a permissive license and may be used
commercially without restriction.** Where a license requires its copyright
notice to be reproduced in binary distributions, that notice is reproduced
below. Full license texts also ship in-tree under each component's `src/<name>`
directory. The running emulator prints these notices itself with
`rp6502-emu --credits` (on the web, append `?credits` to the page URL).

| Component | License (SPDX) | Ships in | Binary notice required |
|---|---|---|---|
| Picocomputer 6502 emulator (this project) | BSD-3-Clause | desktop, web, windows | yes |
| RP6502 firmware subset (audio, terminal + font, VGA modes, `atr`, `rln`, `scanvideo` shim) | BSD-3-Clause | desktop, web | yes |
| emu8950 (OPL audio) | MIT | desktop, web | yes |
| Dear ImGui | MIT | desktop | yes |
| cppdap | Apache-2.0 | desktop | yes |
| nlohmann/json (bundled in cppdap) | MIT | desktop | yes |
| Emscripten runtime + musl libc + Node-derived glue | MIT | web | yes |
| wingetopt | ISC + BSD-2-Clause | windows | yes |
| floooh/chips (incl. the `w65c02.h` fork) | Zlib | desktop, web | no (courtesy) |
| floooh/sokol | Zlib | desktop, web | no (courtesy) |
| FatFs (`ffunicode.c` only) | FatFs license (BSD-1-Clause) | desktop, web | no (binary waived) |

The terminal font (`vga/term/font.c`) is original glyph data described as
"based on the IBM VGA typeface." In the United States bitmap font glyph data is
not copyrightable, so it carries no separate license obligation; it is covered
by the RP6502 firmware's BSD-3-Clause notice. "IBM" is used descriptively only.

---

## MIT License

Applies to **Dear ImGui** (Copyright © 2014-2026 Omar Cornut), **emu8950**
(Copyright © 2001-2020 Mitsutaka Okazaki; © 2021-2022 Graham Sanderson),
**nlohmann/json** (Copyright © 2013-2019 Niels Lohmann), and the **Emscripten**
runtime (Copyright © 2010-2014 Emscripten authors; bundled musl libc © 2005-2020
Rich Felker, et al.; Node-derived portions © Joyent, Inc. and other Node
contributors).

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## Apache License 2.0

Applies to **cppdap** — Copyright © 2019 Google LLC.

> Licensed under the Apache License, Version 2.0 (the "License"); you may not use
> this file except in compliance with the License. You may obtain a copy of the
> License at https://www.apache.org/licenses/LICENSE-2.0
>
> Unless required by applicable law or agreed to in writing, software
> distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
> WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

The full Apache-2.0 text ships in-tree at `vendor/cppdap/LICENSE`. cppdap contains
no `NOTICE` file. The Apache-2.0 patent grant (Section 3) applies.

## BSD 3-Clause License

Applies to the **Picocomputer 6502 emulator** (this project, Copyright © 2026
Rumbledethumps) and the bundled **RP6502 firmware** subset — including the
terminal font and the `scanvideo` host shim, which carries portions
Copyright © 2020 Raspberry Pi (Trading) Ltd. (Pico-SDK-derived; also from
`vga/modes/mode4.c`).

> Copyright (c) 2026 Rumbledethumps
> Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this
>    list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. Neither the name of the copyright holder nor the names of its contributors
>    may be used to endorse or promote products derived from this software
>    without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
> ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
> WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
> FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
> OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## ISC and BSD 2-Clause (Windows build only)

Applies to **wingetopt** — Copyright © 2002 Todd C. Miller (ISC-style) and
Copyright © 2000 The NetBSD Foundation, Inc. (BSD-2-Clause). Full text ships
in-tree at `vendor/wingetopt/LICENSE`.

> Copyright (c) 2002 Todd C. Miller <Todd.Miller@courtesan.com>
>
> Permission to use, copy, modify, and distribute this software for any purpose
> with or without fee is hereby granted, provided that the above copyright
> notice and this permission notice appear in all copies. THE SOFTWARE IS
> PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
> SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
> EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
> CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
> DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
> ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
> SOFTWARE.
>
> Copyright (c) 2000 The NetBSD Foundation, Inc. All rights reserved. This code
> is derived from software contributed to The NetBSD Foundation by Dieter Baron
> and Thomas Klausner. Redistribution and use in source and binary forms, with
> or without modification, are permitted provided that the conditions of the
> BSD 2-Clause license are met. THIS SOFTWARE IS PROVIDED BY THE NETBSD
> FOUNDATION, INC. AND CONTRIBUTORS "AS IS" AND ANY WARRANTIES ARE DISCLAIMED.

## zlib License (courtesy acknowledgment — no binary notice required)

**floooh/chips** and **floooh/sokol** — Copyright © 2018 Andre Weissflog. The
`src/emu/w65c02.h` 65C02 CPU core is a hand-maintained fork of `chips/m6502.h`;
it retains the original copyright, is plainly marked as altered in source, and
remains under the zlib license (satisfying the zlib mark-altered-versions term).

## FatFs (courtesy acknowledgment — binary notice waived)

**FatFs** — Copyright © 2025 ChaN. Only `ffunicode.c` (the Unicode/OEM code-page
tables) is compiled in. The FatFs license expressly waives any documentation
notice for binary redistribution. Full text ships at
`vendor/fatfs/LICENSE.txt`.
