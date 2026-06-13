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

Limit git usage unless I specifically ask. No commits, branches, stashes,
resets, or any other git command on your own initiative.

To build, run `cmake --build build` from the project root. That builds
every target in one shot. Do not hunt for individual ninja target names
(rp6502_ria, rp6502_ria_w, rp6502_vga, etc.) — just build everything.

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

Commit this information to MEMORY.
