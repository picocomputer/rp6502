This is a RP2350 project, not RP2040 as some legacy filenames may suggest.
We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* vendor/tinyusb
* vendor/tinyusb_rp6502/hcd_rp2040.c
* vendor/tinyusb_rp6502/rp2040_usb.c
* vendor/tinyusb_rp6502/midi_host.c

Use Read for file content (not `cat`). When Bash is the right tool, keep
each invocation to a single command. No pipes, no `; echo`, no `2>/dev/null`,
no heredocs — compound commands defeat the permission allowlist matcher
and prompt for approval even when each segment is permitted. Chain via
separate tool calls instead of shell operators.

Never search the root of the filesystem. Everything you need will be in
the user home directory.

Don't spam git history unless specifically asked to. Do not commit or push
unless specifically asked to. Do not look for answers in git history.
Do not fetch git history unless specifically instructed to. Use git to
rename or move files and folders.

To build, run `cmake --build build` from the project root. That
builds every target in one shot. Do not hunt for individual ninja target
names (rp6502-ria, rp6502-ria-w, rp6502-vga, etc.) — just build everything.
The emulator is a separate tree at build/emulator (`cmake --build build/emulator`).

The emulator's wasm/EMSCRIPTEN target IS buildable locally — don't claim it
isn't. The vendored toolchain resolves without sourcing emsdk_env.sh (node is
on PATH): `cmake -S src/emu -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_TOOLCHAIN_FILE=vendor/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`
then `cmake --build build/web` (CI uses the equivalent `wasm` preset from
src/emu). Verify the EMSCRIPTEN branch (host/web/fs.c + host/posix) this way
when you touch it.

Never delete debug macros (DBG, DEBUG_*, etc.) on "currently unused"
grounds. They are scaffolding kept for future bring-up. If a review
notices one isn't called today, leave it; do not propose removing it.

Comments. Default to NOT adding one. Add a comment only for a non-obvious
*why* — never a play-by-play of the *what*. Your AI slop comments contain
ephemeral or incorrect information and make code reviews take longer than
necessary. Specific slop to never write:
* Don't name the current caller ("Called by loc when..."). Callers are a
  grep away and the note rots the moment a second caller appears.
* Don't restate the function name or a parameter ("cp is the code page").
* Don't leak implementation or internal-state conditions into a header
  ("re-applied only while in auto mode"). The header states the contract;
  the .c holds the how.
* Header comments are contract-level and as terse as their neighbors. Match
  the existing one-liners like "// Code page without saving to config".

Docs live in ~/picocomputer.github.io (Sphinx, source in docs/source/*.rst).
When a change alters observable behavior — syscalls/API, device pipes,
monitor commands — update the matching docs in the same change. Match the
existing prose voice; never edit docs/build (generated output).

The vendored FatFs sources (vendor/fatfs/ff.c, ff.h, ffconf.h) are upstream
code you re-apply on every version bump, so editing them is a last resort —
prefer our own files. Two deliberate, sanctioned exceptions already live
there: the formatting "RP6502 mkfs preview hook" (dsk_mkfs_capture) in ff.c,
and the FF_NO_DBCS code-page switch in ffconf.h (honored by ff.c's f_setcp
and DBC range tables, and by ffunicode.c) that drops the DBCS code pages.
Keep any such edit minimal, tag it RP6502, and call it out — don't churn
or "reconcile" markers.

The audio stream is intentionally never cleared or reset. PSG and OPL reset;
the BEL device and anything already in the audio buffer are deliberately NOT
reset or cleared — a rung bell rings through a reset.

Firmware storage I/O is pump-blocking, NOT synchronous. A guest file read
(fat_std_read -> f_read -> disk_read -> msc_scsi_sync) fires the async USB
transfer then spins pumping main_task() (usb/vga/cpu/audio) until it completes —
nothing freezes, and it returns STD_OK in one dispatch (STD_PENDING is console/pipe
drivers only). The emulator can't pump inline (it presents each frame via a discrete
sokol swap after run_frame), so it keeps the system live the opposite way: its host
fs read/write are async and return STD_PENDING, unwinding to the frame loop so the
frame presents while api_task re-polls. Don't call firmware storage "synchronous" —
it pump-blocks.

Commit this information to MEMORY.
