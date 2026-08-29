Picocomputer 6502
=================

Always begin at the documentation:
https://picocomputer.github.io/


Which files
-----------

Every Picocomputer needs two Pi Picos flashed with two different
files. The VGA file is the same for everyone:

    rp6502-vga.uf2

The RIA file depends on which Pi Pico you have. Use the "-w" file
unless you know you have a plain Pi Pico 2; wireless is where the
WiFi, Bluetooth, network time, and modem support live:

    rp6502-ria-w.uf2    Pi Pico 2 W
    rp6502-ria.uf2      Pi Pico 2


Flashing
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

6502 software is distributed as files ending with ".rp6502". Run
them from the console with the "LOAD" command. Find them on Discord,
which has a forum for ROMs, or on itch.io under the RP6502 tag:

    https://discord.gg/TC6X8kTr6d
    https://itch.io/games/tag-rp6502
