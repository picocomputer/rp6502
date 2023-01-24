# FatFs - Generic FAT Filesystem Module

From: http://elm-chan.org/fsw/ff/00index_e.html

The ABI of the RP6502 Operating System is based on CC65 calls to FatFs across the RIA.

## Version

R0.15 w/patch1  (November 6, 2022)

## Config

Plenty of room. Turn everything on as needed.

* #define FF_CODE_PAGE   RP6502_CODE_PAGE
* #define FF_FS_EXFAT    RP6502_EXFAT
* #define FF_LBA64       RP6502_EXFAT
* #define FF_FS_NORTC    1
* #define FF_FS_RPATH    2
* #define FF_LBA64       1
* #define FF_FS_EXFAT    1
* #define FF_USE_LFN     1
* #define FF_FS_LOCK     8
* #define FF_VOLUMES     9

FF_VOLUMES is 9 because volume 0: is unused so drives can match USB device IDs.

## Tables in flash

Plenty of flash for all code pages. Tables in ff.c and ffunicode.c have to be forced there.

* #include "pico/platform.h"
* static const __in_flash("fatfs") ...
