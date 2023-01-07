# FatFs - Generic FAT Filesystem Module

This is the foundation of the RP6502 Operating System.

## Version

R0.15 w/patch1  (November 6, 2022)

## Config

Plenty of room. Turn everything on as needed.

* #define FF_FS_NORTC    1
* #define FF_FS_RPATH    2
* #define FF_CODE_PAGE   0
* #define FF_LBA64       1
* #define FF_FS_EXFAT    1
* #define FF_USE_LFN     1
* #define FF_FS_LOCK     8

## Tables in flash

Plenty of flash for all code pages. Tables in ff.c and ffunicode.c have to be forced there.

* #include "pico/platform.h"
* static const __in_flash("fatfs") ...
