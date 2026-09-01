/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_DIR_H_
#define _OSAL_DIR_H_

/* The eighteen directory syscalls, over whatever drive a machine has.
 *
 * What reaches the 6502 is the same on every machine -- the xstack a path
 * arrives on, the FILINFO an entry leaves in, which errno a bad descriptor is
 * -- so it is written here once. What a drive is differs: FatFs answers with a
 * FRESULT, a host filesystem answers by setting errno, and neither of those
 * spellings appears above this seam, because a backend call reports the way
 * std_driver_t's do -- false, and the api_errno through the out parameter.
 *
 * The entry counter telldir and seekdir are about is this layer's. Both drives
 * kept one, both counted the same things, and a drive that skips "." and ".."
 * on its own gets the counting right by saying nothing about it.
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


/* Machine events: a run starts with no directory open. */
void dir_run(void);
void dir_stop(void);

bool dir_api_stat(void);
bool dir_api_opendir(void);
bool dir_api_readdir(void);
bool dir_api_closedir(void);
bool dir_api_telldir(void);
bool dir_api_seekdir(void);
bool dir_api_rewinddir(void);
bool dir_api_unlink(void);
bool dir_api_rename(void);
bool dir_api_chmod(void);
bool dir_api_utime(void);
bool dir_api_mkdir(void);
bool dir_api_chdir(void);
bool dir_api_chdrive(void);
bool dir_api_getcwd(void);
bool dir_api_setlabel(void);
bool dir_api_getlabel(void);
bool dir_api_getfree(void);

/* This driver's row in a machine's driver list; see core/driver.h. */
#define DIR_DRIVER DRIVER(nul_init, nul_task, nul_task, dir_run, dir_stop, nul_break, nul_config, nul_config)

#endif /* _OSAL_DIR_H_ */
