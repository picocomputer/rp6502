This is a RP2350 project, not RP2040 as some legacy filenames may suggest.

Docs live in ~/picocomputer.github.io (Sphinx, source in docs/source/*.rst).
When a change alters observable behavior — syscalls/API, device pipes,
monitor commands — update the matching docs in the same change. Match the
existing prose voice; never edit docs/build (generated output).

We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* vendor/tinyusb
* vendor/tinyusb_rp6502/hcd_rp2040.c
* vendor/tinyusb_rp6502/rp2040_usb.c
* vendor/tinyusb_rp6502/midi_host.c

We have patterns you must obey. Do not write any code without learning these
patterns. Exports almost always begin with the filename. All drivers have a
lifecycle: init, run, stop, break. All settings have a load, set, get pattern
with a possible run-only state.

We do not waste memory defending against bad code. Every byte is precious but
it's always better to fix bad code even if it's a few more bytes than a hack.

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to. Do not fetch git history unless specifically
instructed to.

To build firmware, run `cmake --build build` from the project root. That
builds every target in one shot. Do not test non-W Pico builds.

The emulator is a separate tree at build/emulator (`cmake --build build/emulator`).
Use the vendored wasm/EMSCRIPTEN toolchain for web builds.

Comments. Default to NOT adding one. Add a comment only for a non-obvious
*why* — never a play-by-play of the *what*. Commentary about work in progress
must never be added.
