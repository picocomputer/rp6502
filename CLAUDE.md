This is a RP2350 project, not RP2040 as some legacy filenames may suggest.
We do not use the TinyUSB in the Pi Pico SDK.
We have a submodule with overrides:
* src/tinyusb
* src/tinyusb_rp6502/hcd_rp2040.c
* src/tinyusb_rp6502/rp2040_usb.c
* src/tinyusb_rp6502/msc_host.c

Before every Bash call, ask: can Grep, Glob, or Read do this? If yes, use
those. Bash is reserved for things that must be shell.

Never search the root of the filesystem. Everything you need will be in
the user home directory.
