Picocomputer 6502 Emulator
==========================

Always begin at the documentation:
https://picocomputer.github.io/


Install
-------

Drag rp6502-emu.app to your Applications folder. This is an Apple
silicon (arm64) build and requires macOS 11 or later.


Unsigned
--------

The app is not signed and not notarized. Gatekeeper blocks the
first launch. Allow it under System Settings > Privacy & Security >
"Open Anyway", or remove the quarantine flag yourself:

    xattr -dr com.apple.quarantine rp6502-emu.app


Running software
----------------

The emulator takes a ROM file as an argument or you can drag and
drop a ROM on to the window.

6502 software is distributed as files ending with ".rp6502". Find
them on Discord, which has a forum for ROMs, or on itch.io under
the RP6502 tag:

    https://discord.gg/TC6X8kTr6d
    https://itch.io/games/tag-rp6502
