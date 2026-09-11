/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_DIR_H_
#define _OSAL_DIR_H_

#include "core/api/api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIR_MAX_OPEN 8

#define F_NAME_MAX 255   /* fname, the long name */
#define F_ALTNAME_MAX 12 /* altname, the 8.3 short name where there is one */

typedef struct
{
    uint32_t fsize;
    uint16_t fdate; /* DOS date: (year-1980)<<9 | month<<5 | day */
    uint16_t ftime; /* DOS time: hour<<11 | minute<<5 | second/2 */
    uint16_t crdate;
    uint16_t crtime;
    uint8_t fattrib; /* RDO 0x01 HID 0x02 SYS 0x04 DIR 0x10 ARC 0x20 */
    char altname[F_ALTNAME_MAX + 1];
    char fname[F_NAME_MAX + 1];
} f_stat_t;

bool drive_stat(const char *path, f_stat_t *info, api_errno *err);
bool drive_unlink(const char *path, api_errno *err);
bool drive_rename(const char *oldname, const char *newname, api_errno *err);
bool drive_mkdir(const char *path, api_errno *err);
bool drive_chdir(const char *path, api_errno *err);
bool drive_chdrive(const char *drive, api_errno *err);
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err);
bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect, api_errno *err);
bool drive_getcwd(char *buf, size_t size, api_errno *err);
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err);
bool drive_setlabel(const char *path, api_errno *err);
bool drive_opendir(const char *path, int *des, api_errno *err);
bool drive_readdir(int des, f_stat_t *info, api_errno *err);
bool drive_closedir(int des, api_errno *err);
bool drive_rewinddir(int des, api_errno *err);
bool drive_validate(int des, api_errno *err);

/* drive_dir_path answers with an absolute path, because a savestate load
 * restores the working directory separately and the guest may have moved it
 * since. drive_reopendir does not restore the read position; core/api/dir.c
 * winds the entry count it kept. */
bool drive_dir_path(int des, char *buf, size_t size);
bool drive_reopendir(int des, const char *path, api_errno *err);
void oem_fs_code_page(uint16_t cp);

char *os_dir_realpath(const char *path);

/* A copy of a path that stays valid until os_dir_path_drop. A board with no
 * heap answers from static buffers and returns NULL for a path that will not
 * fit one. */
char *os_dir_path_hold(const char *path);
void os_dir_path_drop(char *path);

#endif /* _OSAL_DIR_H_ */
