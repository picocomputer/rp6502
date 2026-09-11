This is a RP2350 project, not RP2040 as some legacy filenames may suggest.

A machine is everything that comes tgether to make a Picocomputer.
src/host brings together the core with the needed services.
src/osal connects the core with an operating system.
src/core is the bulk of what makes a Picocomputer.

We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* vendor/tinyusb
* vendor/tinyusb_rp6502/hcd_rp2040.c
* vendor/tinyusb_rp6502/rp2040_usb.c
* vendor/tinyusb_rp6502/midi_host.c

We do not waste memory defending against bad code. Every byte is precious but
it's always better to fix bad code even if it's a few more bytes than a hack.

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to. Do not fetch git history unless specifically
instructed to.

Never run a test that drives real hardware without being asked to, for a
stated reason, that time. The `pico` ctest label is that hardware: it is one
attached board on a serial port, it is slow, and it fails for reasons that
have nothing to do with the change. It exists only in a desktop tree, so run
`ctest --test-dir build/sokol -LE pico`. Never flash a board unasked either.

Comments. Default to NOT adding one. Add a comment only for a non-obvious
*why* — never a play-by-play of the *what*. Commentary about work in progress
must never be added. Provide an understanding, not a narration.

Don't load up plans with narration and justification. A plan describes the
work you intend to do. A plan is not a whitepaper, datasheet, or story. A
plan describes the work you want to do and where it lands.
