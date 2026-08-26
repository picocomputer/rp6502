Picocomputer 6502 - libretro core
=================================

Always begin at the documentation:
https://picocomputer.github.io/


Install
-------

Most people never need this file. RetroArch's Online Updater has
the core under "Picocomputer 6502", and installing it that way
keeps it up to date.

This zip holds the core for every platform we build, a folder
each:

    linux-x86_64/rp6502_libretro.so
    linux-aarch64/rp6502_libretro.so
    windows-x86_64/rp6502_libretro.dll
    macos-arm64/rp6502_libretro.dylib
    android-arm64/rp6502_libretro.so

Take the one for your machine, put it in the directory your
frontend keeps its cores in, and put rp6502_libretro.info beside
it in the info directory. RetroArch prints both paths under
Settings / Directory; on Linux they are usually:

    ~/.config/retroarch/cores
    ~/.config/retroarch/info

You can also load it without installing it at all:

    retroarch -L linux-x86_64/rp6502_libretro.so program.rp6502


Running software
----------------

6502 software is distributed as files ending with ".rp6502". Load
one as content the way you would a cartridge. Find them on
Discord, which has a forum for ROMs, or on itch.io under the
RP6502 tag:

    https://discord.gg/TC6X8kTr6d
    https://itch.io/games/tag-rp6502

The Picocomputer is a computer, so a program may want a keyboard,
a mouse, or up to four gamepads.

The keyboard needs one setting. RetroArch binds keys to its own
controller and hotkeys - Enter is Start, "p" pauses - so until you
turn that off, typing does not reach the program. Press Scroll
Lock for Game Focus, and the whole keyboard is the computer's.

To have it on every time, set Settings / Input / Auto Enable Game
Focus to "Detect". This core tells RetroArch it wants a keyboard,
which is what that setting looks for.

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
