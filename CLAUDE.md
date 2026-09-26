This is a RP2350 project, not RP2040 as some legacy filenames may suggest.

A machine is everything that comes together to make a Picocomputer.
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

Comments. Default to NOT adding one. Add a comment only for a non-obvious
*why* — never a play-by-play of the *what*. Commentary about work in progress
must never be added. Provide an understanding, not a narration, and only
when reading the code doesn't convey the message. Short, one-line, comments
describing an area to assist with navigation are engouraged.

Voice. In every comment, doc, plan, commit message and reply, write about
code, programs, machines, devices, files and data as objects, never as beings:
they do not know, want, ask, say, tell, see, reach, decide, carry, own or
arrive, and nothing belongs to them or sits behind anything. Use plain words
in full sentences with explicit subjects and no figures of speech. A comment
gives only the why that cannot be read from the code, never what the code
does. Never ignore this paragraph, in any session or subagent, for any reason.
