Picocomputer 6502
=================

Always begin here at the documentation:
https://picocomputer.github.io/


Firmware
--------

Version 0.25 and later has a flash command you can use from the
RP6502-RIA monitor. Put the .uf2 files on a USB drive and plug
it into your Picocomputer 6502. You'll need to flash both files;
the system will reboot after each flash:

    FLASH rp6502-ria-w.uf2
    FLASH rp6502-vga.uf2

If you have a new system or the flash command fails, you must use
the BOOTSEL method. Hold down the button labeled BOOTSEL on your
Pi Pico while plugging it into a computer. The Pi Pico will look
like a drive with a FAT filesystem. Do this for both Pi Picos.
For example, on Windows you might use:

    COPY rp6502-ria-w.uf2 X:
    COPY rp6502-vga.uf2 X:

It is not possible to "brick" a Pi Pico from a failed flash. You
can always recover with the BOOTSEL method.


ROMs
----

6502 software is typically distributed as a ROM file ending with ".rp6502".
These can be run from the console with the "LOAD" command or run with the
emulator.

Discord has a forum for ROMs and publishing on itch.io is recommended if
you want to reach a larger audience. The documentation has a discord invite.


Emulator
--------

This zip contains snapshot builds of the Picocomputer 6502 emulator.
The desktop emulators take a ROM file as an argument or you can drag
and drop a ROM on to the window.

Windows: rp6502-emu.exe is an unsigned native x64 build. SmartScreen
may warn on first launch; choose "More info" then "Run anyway".
Requires a GPU with Direct3D 11.

MacOS: rp6502-emu.app is unsigned and not notarized. Gatekeeper blocks
the first launch; allow it under System Settings > Privacy & Security >
"Open Anyway", or remove the quarantine flag:

    xattr -dr com.apple.quarantine rp6502-emu.app

Linux: rp6502-emu-x86_64 and rp6502-emu-aarch64 are built on Ubuntu
22.04 and need glibc 2.35+ plus GL/X11/ALSA runtime libraries.

Android: rp6502-emu.apk is signed with a debug key. Android will warn
on installation; choose "More info" then "Install anyway".

Web: itch.io.zip is the ready-to-publish itch.io sample; it contains a
README for publishing on itch.io.
