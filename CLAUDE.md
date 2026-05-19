This is a RP2350 project, not RP2040 as some legacy filenames may suggest.
We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* src/tinyusb
* src/tinyusb_rp6502/hcd_rp2040.c
* src/tinyusb_rp6502/rp2040_usb.c
* src/tinyusb_rp6502/msc_host.c

Use Read for file content (not `cat`). When Bash is the right tool, keep
each invocation to a single command. No pipes, no `; echo`, no `2>/dev/null`,
no heredocs — compound commands defeat the permission allowlist matcher
and prompt for approval even when each segment is permitted. Chain via
separate tool calls instead of shell operators.

Never search the root of the filesystem. Everything you need will be in
the user home directory.

To build, run `cmake --build build` from the project root. That builds
every target in one shot. Do not hunt for individual ninja target names
(rp6502_ria, rp6502_ria_w, rp6502_vga, etc.) — just build everything.

Never delete debug macros (DBG, DEBUG_*, etc.) on "currently unused"
grounds. They are scaffolding kept for future bring-up. If a review
notices one isn't called today, leave it; do not propose removing it.
