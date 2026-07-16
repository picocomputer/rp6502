# Installing Picocomputer 6502 firmware.

Version 0.25 and later have a flash command you can use from the
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
