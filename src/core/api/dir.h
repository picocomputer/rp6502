/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_DIR_H_
#define _CORE_API_DIR_H_

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
 * kept one, both counted the same things, and a backend that skips "." and
 * ".." on its own gets the counting right by saying nothing about it.
 */

#include "core/api/api.h"
#include "fatfs/ff.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No drive opens more than this many directories at once. */
#define DIR_MAX_OPEN 8

/* A drive, as the directory syscalls need it. Every call answers true, or
 * false with *err set, and none of them touches the xstack.
 *
 * A slot left NULL is a call this drive does not have, and answers ENOSYS --
 * the same convention std_driver_t uses. A drive that has an *answer* but no
 * feature says so itself: the host filesystem has no volume label, and reports
 * an empty one rather than a missing call, so label-aware programs run. */
typedef struct
{
    bool (*stat)(const char *path, FILINFO *fno, api_errno *err);
    bool (*unlink)(const char *path, api_errno *err);
    bool (*rename)(const char *oldname, const char *newname, api_errno *err);
    bool (*mkdir)(const char *path, api_errno *err);
    bool (*chdir)(const char *path, api_errno *err);
    bool (*chdrive)(const char *drive, api_errno *err);
    bool (*chmod)(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
    /* fdate/ftime, and crdate/crtime for a drive that stores them. */
    bool (*utime)(const char *path, const FILINFO *fno, api_errno *err);
    bool (*getfree)(const char *path, uint32_t *tot_sect, uint32_t *fre_sect, api_errno *err);
    /* The whole drive-qualified path, as the 6502 spells it. */
    bool (*getcwd)(char *buf, size_t size, api_errno *err);
    bool (*getlabel)(const char *path, char *label, size_t size, api_errno *err);
    bool (*setlabel)(const char *path, api_errno *err);

    /* *des is this drive's own index, below DIR_MAX_OPEN. */
    bool (*opendir)(const char *path, int *des, api_errno *err);
    /* fno->fname[0] == 0 is end-of-directory, which is not an error. Skipping
     * "." and ".." is the drive's, since only it knows whether it has them. */
    bool (*readdir)(int des, FILINFO *fno, api_errno *err);
    bool (*closedir)(int des, api_errno *err);
    bool (*rewinddir)(int des, api_errno *err);
    /* EINVAL out of range, EBADF not open. */
    bool (*validate)(int des, api_errno *err);
} dir_backend_t;

/* This machine's drive, supplied by its main.c. */
const dir_backend_t *main_dir_backend(void);


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

#endif /* _CORE_API_DIR_H_ */
