# FatFs - Generic FAT Filesystem Module

From: http://elm-chan.org/fsw/ff

This copy is FatFs R0.16 with patches 1 and 2 to ff.c, and with the
ffunicode.c of August 3, 2025, from http://elm-chan.org/fsw/ff/patches.html.

Each change in ff.c, ff.h, and ffunicode.c has a comment that contains
"RP6502". After an upgrade of FatFs, apply each change again.

## ffconf.h

ffconf.h holds the configuration of the Picocomputer. It also has these
additions:

- `FF_NO_DBCS` 1, with `FF_CODE_PAGE` 0, removes the DBCS code pages 932,
  936, 949, and 950 from ff.c and ffunicode.c. Their tables are most of the
  size of FatFs.
- ffconf.h declares `f_getldnumber()`.
- `FF_MIN_GPT` calls `drive_min_gpt()`, which ffconf.h declares. The disk
  monitor uses this call to select MBR or GPT when it formats a drive.
  `FF_MIN_GPT` has an effect only when `FF_LBA64` is 1.
- `FF_LBA64` and `FF_FS_EXFAT` take the value of `RP6502_EXFAT`. Every
  build sets `RP6502_EXFAT` to 0.
- ffconf.h does not define `FF_VOLUME_STRS`, so the volume names come from
  the table `VolumeStr` in src/host/pico/ria/usb/msc.c.
- `FF_FS_LOCK` is the sum of the objects that can be open at once.
  src/osal/pico/fs.c and src/host/pico/ria/usb/msc.c check the sum.

## ff.h

- `FRESULT` has two more results, `FR_NO_SPACE` (20) and `FR_NOT_EMPTY`
  (21). Upstream FatFs gives `FR_DENIED` for both conditions, and for a
  refused access too. src/osal/pico/errmap.c maps `FR_NO_SPACE` to ENOSPC
  and `FR_NOT_EMPTY` to EEXIST, as POSIX does, and `FR_DENIED` stays
  EACCES. src/host/pico/ria/mon/mon.c has a message for each.

## ff.c

- A full drive gives `FR_NO_SPACE`, not `FR_DENIED`: `dir_next()` when no
  cluster is free to stretch a directory, `dir_alloc()` when no directory
  entry is free, and `f_mkdir()` when no cluster is free for the new
  directory. `f_open()` with a create, `f_mkdir()`, `f_rename()`, and
  `f_setlabel()` get the result from these functions.
- `f_unlink()` of a directory that is not empty gives `FR_NOT_EMPTY`, and
  `f_unlink()` of the current directory gives `FR_LOCKED`, which maps to
  EBUSY. Upstream FatFs gives `FR_DENIED` for both. A read-only entry still
  gives `FR_DENIED`.
- `f_open()` with `FA_OPEN_ALWAYS` refuses a read-only file only when the
  open writes, with `FA_WRITE` or `FA_CREATE_ALWAYS`. Upstream FatFs
  refuses every such open of a read-only file, so open(2) with
  O_RDONLY|O_CREAT of a read-only file failed. A directory is still always
  refused.

- `f_rename()` refuses to move a directory into itself, and returns
  `FR_INVALID_NAME`. rename(2) refuses the same move with EINVAL. Upstream
  FatFs moves the directory. After that move, no path reaches the
  directory or its contents, and their clusters stay allocated. The new
  function `chk_ancestor()` does the check before `f_rename()` tests
  whether the new name exists, so a refused rename changes nothing. On
  FAT12, FAT16, and FAT32, `chk_ancestor()` reads the ".." entries from the
  new parent directory up to the root. exFAT has no ".." entries, so on
  exFAT, `chk_ancestor()` searches the path that `follow_path()` records in
  `fs->xcwds2`. The exFAT check requires `FF_FS_RPATH` 1 or 2.
- On exFAT, `f_rename()` renames a directory in the path of the current
  directory, as on FAT. Upstream FatFs refuses this rename with
  `FR_DENIED`. On exFAT, FatFs keeps the path of the current directory in
  `fs->xcwds`, as a list of the directories from the root. A rename makes
  this list incorrect. After the rename, `f_rename()` builds the list again
  from two parts: the path that `follow_path()` records for the new name,
  and the part of the old list from the renamed directory to the current
  directory. The update reads nothing from the disk. A rename that puts the
  current directory deeper than `FF_PATH_DEPTH` fails with
  `FR_NOT_ENOUGH_CORE` and changes nothing. The new function
  `xcwds2_parent()` finds the new parent directory in the recorded path.
  This change and `chk_ancestor()` use it.
- `f_getldnumber()` makes the static `get_ldnumber()` available to callers.
  It returns the logical drive number of a path. For a path without a
  drive, it returns the number of the current drive. For a drive that does
  not exist, it returns -1. src/host/pico/ria/sys/path.c and
  src/osal/pico/dir.c use it.
- `f_mkfs()` calls `drive_mkfs_capture()` after it selects the FAT type and
  the cluster size, and before it writes or trims. A nonzero return ends
  `f_mkfs()` with `FR_OK` before any write. The disk monitor uses this call
  to preview a format. In the exFAT branch, the TRIM is after the call, so
  a preview does not trim. src/host/pico/ria/mon/drive.c defines
  `drive_mkfs_capture()`.
- The range check of `FF_MIN_GPT` is a comment, because the preprocessor
  cannot evaluate a function call.
- `FF_NO_DBCS` removes the DBCS range tables, and the DBCS entries of the
  code page table of `f_setcp()`.

## ffunicode.c

- `FF_NO_DBCS` removes the DBCS conversion tables, and the conversion
  functions use only the SBCS tables. A DBCS `FF_CODE_PAGE` with
  `FF_NO_DBCS` stops the compile with an error.
