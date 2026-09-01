/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_DIR_H_
#define _OSAL_DIR_H_

/* What a drive is, under the directory syscalls. FatFs answers with a FRESULT
 * and a host filesystem by setting errno; neither spelling appears here,
 * because a backend call reports the way std_driver_t's do -- false, and the
 * api_errno through the out parameter.
 *
 * What the 6502 asks of these is core/api/dir.h, which is written once for
 * every machine.
 */

#include "core/api/api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No drive opens more than this many directories at once. */
#define DIR_MAX_OPEN 8

/* An entry, as the 6502 receives it. This is the SDK's f_stat_t, which
 * rp6502.h declares and the OS documentation spells out under STAT -- the
 * same eight fields, in the order dir.c pushes them.
 *
 * The shape is FAT's: attribute bits, and a date and time packed the way DOS
 * packed them. That is the API's own vocabulary and predates any of the
 * drives; a machine with a FAT volume reads it off the medium, and one
 * without builds it from what its OS keeps. Neither needs FatFs's header to
 * say so, which is why this is here rather than borrowed from it. */
#define F_NAME_MAX 255   /* fname, the long name */
#define F_ALTNAME_MAX 12 /* altname, the 8.3 short name where there is one */

typedef struct
{
    uint32_t fsize;
    uint16_t fdate; /* DOS date: (year-1980)<<9 | month<<5 | day */
    uint16_t ftime; /* DOS time: hour<<11 | minute<<5 | second/2 */
    uint16_t crdate;
    uint16_t crtime;
    uint8_t fattrib; /* FAT attributes: RDO 0x01 HID 0x02 SYS 0x04 DIR 0x10 ARC 0x20 */
    char altname[F_ALTNAME_MAX + 1];
    char fname[F_NAME_MAX + 1];
} f_stat_t;

/* This machine's drive: one set of functions, defined by whichever host is
 * linked. There is one drive per machine and there will not be two, so a
 * table of pointers would be an indirection on every syscall standing in for
 * a choice nobody makes -- and one the compiler cannot see through.
 *
 * Every call answers true, or false with *err set, and none of them touches
 * the xstack. A drive that cannot do one of these says so itself, returning
 * false with API_ENOSYS; a drive that has an *answer* but no feature answers
 * -- a host filesystem has no volume label, and reports an empty one rather
 * than a missing call, so label-aware programs run. */
bool drive_stat(const char *path, f_stat_t *info, api_errno *err);
bool drive_unlink(const char *path, api_errno *err);
bool drive_rename(const char *oldname, const char *newname, api_errno *err);
bool drive_mkdir(const char *path, api_errno *err);
bool drive_chdir(const char *path, api_errno *err);
bool drive_chdrive(const char *drive, api_errno *err);
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
/* fdate/ftime, and crdate/crtime for a drive that stores them. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err);
bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect, api_errno *err);
/* The whole drive-qualified path, as the 6502 spells it. */
bool drive_getcwd(char *buf, size_t size, api_errno *err);
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err);
bool drive_setlabel(const char *path, api_errno *err);

/* *des is this drive's own index, below DIR_MAX_OPEN. */
bool drive_opendir(const char *path, int *des, api_errno *err);
/* info->fname[0] == 0 is end-of-directory, which is not an error. Skipping
 * "." and ".." is the drive's, since only it knows whether it has them. */
bool drive_readdir(int des, f_stat_t *info, api_errno *err);
bool drive_closedir(int des, api_errno *err);
bool drive_rewinddir(int des, api_errno *err);
/* EINVAL out of range, EBADF not open. */
bool drive_validate(int des, api_errno *err);

/* Tell this machine's drive which code page its filenames are in. FatFs keeps
 * one of its own and must be told; a host filesystem takes the bytes as they
 * come and has no page to set. Only pages core/str/unicode.c carries reach
 * here. Declared beside the drive it speaks to rather than in core/str, which
 * has no business declaring a drive contract. */
void oem_fs_code_page(uint16_t cp);


/* An absolute path, which is what argv[0] needs to survive a chdir. Resolved
 * against the same cwd drive_getcwd answers for, which is why it is here and
 * not with the opens. Spelled the way the 6502 spells it, both ways.
 *
 * It allocates its answer, because how long a path the OS will hand back is
 * the OS's to decide and not a caller's to guess. The caller frees; NULL is a
 * path that does not resolve. */
char *os_dir_realpath(const char *path);

#endif /* _OSAL_DIR_H_ */
