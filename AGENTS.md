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

The build runs on Linux, macOS and Windows hosts. src/core/build.cmake and
src/core/build.py hold what differs between them; reach for them first, and
move code into them when it would make a script easier to read.
- Run Python as ${RP6502_PYTHON} SCRIPT, never python3.
- A linker script places by section name: .bss.font8 is font8. Only the RIA
  places whole files by object, through rp6502_object_patterns.
- A source whose symbols or object a linker script names goes through
  rp6502_linker_named, and every placement has an ASSERT or a check that
  fails the build when it matches nothing.
- CMake hands a script what it needs, rather than the script digging it out
  of compiler command lines.
