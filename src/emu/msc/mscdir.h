/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Directory enumeration + file metadata + path mutation (mscdir.c, host/MSC0:
 * only — the firmware routes these straight to FatFs, so there is one backing,
 * no driver table). All 0/-1 with errno; dir.c marshals these to the 6502.
 */

#ifndef _EMU_MSCDIR_H_
#define _EMU_MSCDIR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Path mutation. */
int fs_unlink(const char *path);
int fs_rename(const char *oldp, const char *newp);
int fs_mkdir(const char *path);

/* One stat/dirent in the firmware FILINFO field set (FatFs-packed dates and
 * attribute bits) so dir.c can marshal it to the 6502 byte-for-byte. */
typedef struct
{
    uint32_t size;
    uint16_t mdate, mtime; /* modification date/time, FAT-packed */
    uint16_t cdate, ctime; /* creation date/time, FAT-packed */
    uint8_t attrib;        /* FatFs AM_* bits */
    char altname[13];      /* 8.3 short name (empty on the host) */
    char name[256];        /* long name; name[0]==0 marks end-of-directory */
} fs_info_t;

/* opendir returns a descriptor 0-7, telldir the entry index. */
int fs_stat(const char *path, fs_info_t *info);
int fs_opendir(const char *path);
int fs_readdir(int des, fs_info_t *info);
int fs_closedir(int des);
long fs_telldir(int des);
int fs_seekdir(int des, long off);
int fs_rewinddir(int des);
int fs_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors);
int fs_chmod(const char *path, uint8_t attr, uint8_t mask);
int fs_utime(const char *path, uint16_t fdate, uint16_t ftime);
int fs_getlabel(const char *path, char *label, size_t sz);
int fs_setlabel(const char *path);

void mscdir_reset(void); /* close open directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MSCDIR_H_ */
