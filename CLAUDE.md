This is a RP2350 project, not RP2040 as some legacy filenames may suggest.
We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* src/tinyusb
* src/tinyusb_rp6502/hcd_rp2040.c
* src/tinyusb_rp6502/rp2040_usb.c

Use Read for file content (not `cat`). When Bash is the right tool, keep
each invocation to a single command. No pipes, no `; echo`, no `2>/dev/null`,
no heredocs — compound commands defeat the permission allowlist matcher
and prompt for approval even when each segment is permitted. Chain via
separate tool calls instead of shell operators.

Never search the root of the filesystem. Everything you need will be in
the user home directory.

Limit git usage to as few requests as possible unless I specifically ask
for something only git can answer.

To build, run `cmake --build build` from the project root. That builds
every target in one shot. Do not hunt for individual ninja target names
(rp6502_ria, rp6502_ria_w, rp6502_vga, etc.) — just build everything.

This is a copy_to_ram build: .text and .data live in RAM, only __in_flash
data stays in flash. RAM is the scarce resource. A string literal reaches
flash only as a NAMED, EXTERNAL-linkage __in_flash array. It silently
falls back to RAM if it is `static` (LTO merges it into .rodata.str,
dropping the section) or anonymous (a bare "..." in an array/struct
initializer has no section at all). Pointer/int arrays are exempt and may
stay static. Verify with `objcopy --only-section=.data <elf> f && grep -a`;
don't trust the attribute.

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

Commit this information to MEMORY.
