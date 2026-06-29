Do not modify the vendored deps vendor/chips, vendor/sokol, or vendor/cppdap
without first asking for special permission.

The firmware sources (src/ria, src/vga, ...) are shared with the firmware in
this same repo — the old src/rp6502 submodule is gone. The emulator compiles a
subset of them against the shim headers in src/emu/shim. Changes to the
firmware that expose better interfaces for the emulator are ok, but they affect
the real firmware too; keep concerns separated.

Documentation for the RP6502 components and hardware layout can be found
at ~/picocomputer.github.io which you may edit if you find errors.
