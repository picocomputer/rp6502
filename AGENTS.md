It's always worth the tokens to read ~/picocomputer.github.io at the start
of every session.

This is a RP2350 project, not RP2040 as some legacy filenames may suggest.

A machine is everything that comes together to make a Picocomputer.
src/host brings together the core with the needed services.
src/osal connects the core with an operating system.
src/core is the bulk of what makes a Picocomputer.

We do not use the TinyUSB in the Pi Pico SDK.
We have overrides for many of our submodules. See: vendor/*_rp6502

We do not waste memory defending against bad code. Every byte is precious but
it's always better to fix bad code even if it's a few more bytes than a hack.
Prefer improving the architecture over a wrapper or other hack.

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to.

Hardware on the bench is reached through CMSIS-DAP debug probes, one or two,
each on the SWD port of the RIA or the VGA. Flash only through OpenOCD with
src/host/pico/check.cfg: it reads the program name of the firmware on the chip
(RP6502-VGA, RP6502-RIA, RP6502-RIA-W) and writes nothing unless it matches the
ELF. With two probes, pick one with `adapter serial`. The VGA's own USB port is
a separate serial device. Some benches also have video and audio capture
devices. Under WSL, devices come through usbipd; if one is missing, check
`usbipd.exe list` and attach it.
