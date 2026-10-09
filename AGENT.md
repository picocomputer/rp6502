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

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to.

Comments. Do not narrate the session in comments.

Voice. Machines and software are not beings. Use plain words in full sentences
with explicit subjects and no figures of speech.
