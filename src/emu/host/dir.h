/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: on the native host filesystem: path addressing plus the directory +
 * file-metadata ops (stat, the opendir/readdir family, free space, unlink/
 * rename/mkdir/chmod/utime/label, the cwd). The ops fill the same FatFs FILINFO
 * the 6502 reads, so they are one interchangeable backend (host_dir_ops) the
 * emulator plugs into its runtime dir vtable — the FatFs backend (fat_dir_ops,
 * the shared ria/api/fat.c) is the other. "MSC0:" maps straight onto the OS
 * filesystem: "MSC0:/x" is native "/x", "MSC0:x" is relative to the process cwd,
 * "MSC0://C/x" is the Windows drive "C:/x".
 */

#ifndef _EMU_HOST_DIR_H_
#define _EMU_HOST_DIR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api/api.h"  /* api_errno */
#include "fatfs/ff.h" /* FILINFO */

#ifdef __cplusplus
extern "C"
{
#endif

/* Path addressing (host-only; keeps the fs_ names used by main.c/install.c). */
bool fs_to_host(const char *path, char *host, size_t hsz);            /* MSC0: -> host path */
size_t fs_host_to_msc(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool fs_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *fs_strip_drive(const char *path); /* path past a recognized drive prefix */

/* A directory/metadata backend the emulator plugs into its runtime vtable. Every
 * op returns 0/-1 and sets *err (an api_errno) on failure; stat/readdir fill a
 * FILINFO (fname[0]==0 marks end-of-directory), the same record the 6502 reads. */
typedef struct
{
    int (*stat)(const char *path, FILINFO *fno, api_errno *err);
    int (*opendir)(const char *path, api_errno *err); /* >=0 descriptor, or -1 */
    int (*readdir)(int des, FILINFO *fno, api_errno *err);
    int (*closedir)(int des, api_errno *err);
    int (*telldir)(int des, int32_t *pos, api_errno *err);
    int (*seekdir)(int des, int32_t offs, api_errno *err);
    int (*rewinddir)(int des, api_errno *err);
    int (*unlink)(const char *path, api_errno *err);
    int (*rename)(const char *oldp, const char *newp, api_errno *err);
    int (*chmod)(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
    int (*utime)(const char *path, const FILINFO *fno, api_errno *err);
    int (*mkdir)(const char *path, api_errno *err);
    int (*chdir)(const char *path, api_errno *err);
    int (*chdrive)(const char *path, api_errno *err);
    int (*getcwd)(char *buf, size_t size, api_errno *err);
    int (*getlabel)(const char *path, char *label, api_errno *err); /* label >= 12 bytes */
    int (*setlabel)(const char *path, api_errno *err);
    int (*getfree)(const char *path, uint32_t *free_sectors, uint32_t *total_sectors, api_errno *err);
    void (*stop)(void); /* close open directories (machine reset) */
} fs_dir_ops;

extern const fs_dir_ops host_dir_ops; /* the native host backend */

/* The emulator's active dir backend, swapped at runtime by the drive lifecycle. */
void emu_set_dir_ops(const fs_dir_ops *ops);
void emu_dir_stop(void); /* close open directories on the active backend */

/* The host backend ops (host_dir_ops's members; also called directly by tests). */
int host_stat(const char *path, FILINFO *fno, api_errno *err);
int host_opendir(const char *path, api_errno *err);
int host_readdir(int des, FILINFO *fno, api_errno *err);
int host_closedir(int des, api_errno *err);
int host_telldir(int des, int32_t *pos, api_errno *err);
int host_seekdir(int des, int32_t offs, api_errno *err);
int host_rewinddir(int des, api_errno *err);
int host_unlink(const char *path, api_errno *err);
int host_rename(const char *oldp, const char *newp, api_errno *err);
int host_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
int host_utime(const char *path, const FILINFO *fno, api_errno *err);
int host_mkdir(const char *path, api_errno *err);
int host_chdir(const char *path, api_errno *err);
int host_chdrive(const char *path, api_errno *err);
int host_getcwd(char *buf, size_t size, api_errno *err);
int host_getlabel(const char *path, char *label, api_errno *err);
int host_setlabel(const char *path, api_errno *err);
int host_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors, api_errno *err);
void host_dir_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_DIR_H_ */
