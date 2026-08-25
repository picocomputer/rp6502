Picocomputer 6502 - libretro core
=================================

Always begin at the documentation:
https://picocomputer.github.io/


Install
-------

Most people never need this file. RetroArch's Online Updater has
the core under "Picocomputer 6502", and installing it that way
keeps it up to date.

To install this build by hand, put rp6502_libretro.so in the
directory your frontend keeps its cores in, and put
rp6502_libretro.info beside it in the info directory. RetroArch
prints both paths under Settings / Directory.

    ~/.config/retroarch/cores
    ~/.config/retroarch/info

You can also load it without installing it at all:

    retroarch -L rp6502_libretro.so program.rp6502


Running software
----------------

6502 software is distributed as files ending with ".rp6502". Load
one as content the way you would a cartridge. Find them on
Discord, which has a forum for ROMs, or on itch.io under the
RP6502 tag:

    https://discord.gg/TC6X8kTr6d
    https://itch.io/games/tag-rp6502

The Picocomputer is a computer, so a program may want a keyboard,
a mouse, or up to four gamepads. RetroArch sends the keyboard to
the core only while Game Focus is on - press Scroll Lock to
toggle it, or the hotkey your frontend uses.

A program's saves go to the save directory your frontend has
chosen for it.


What this core does not do
--------------------------

There is no monitor, no debugger and no scripting here: this core
plays a program and stops when the program does. The desktop
emulator has all three, and the same programs run on it:

    https://github.com/picocomputer/rp6502/releases

Save states are not implemented. The core says so, so rewind and
netplay will not offer themselves.
