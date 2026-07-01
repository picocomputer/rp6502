/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: on the native host filesystem: path addressing plus the directory +
 * file-metadata ops (stat, the opendir/readdir family, free space, unlink/
 * rename/mkdir/chmod/utime/label, the cwd). host_* fill the FatFs FILINFO the 6502
 * reads. The emu's host_dir_api_* syscall handlers (emu/api/dir.c) marshal over
 * these; the OP dispatcher installs them for the default host drive and swaps in
 * the real firmware dir_api_* (ria/api/dir.c) on --tmpdrive. "MSC0:" maps straight
 * onto the OS filesystem: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a
 * Windows drive.
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

/* Point the OP dispatcher's dir slots at the FatFs firmware handlers (true, over the
 * RAM disk) or the host handlers (false). Called by the drive lifecycle. */
void emu_dir_ops_set(bool fat);

/* The emu's HOST dir syscall handlers (emu/api/dir.c), installed in the OP array. */
bool host_dir_api_stat(void);
bool host_dir_api_opendir(void);
bool host_dir_api_readdir(void);
bool host_dir_api_closedir(void);
bool host_dir_api_telldir(void);
bool host_dir_api_seekdir(void);
bool host_dir_api_rewinddir(void);
bool host_dir_api_unlink(void);
bool host_dir_api_rename(void);
bool host_dir_api_chmod(void);
bool host_dir_api_utime(void);
bool host_dir_api_mkdir(void);
bool host_dir_api_chdir(void);
bool host_dir_api_chdrive(void);
bool host_dir_api_getcwd(void);
bool host_dir_api_setlabel(void);
bool host_dir_api_getlabel(void);
bool host_dir_api_getfree(void);

/* The host backend (emu/host/dir.c): the actual POSIX work the handlers marshal.
 * Every op returns 0/-1 and sets *err (an api_errno); stat/readdir fill a FILINFO
 * (fname[0]==0 marks end-of-directory). Also called directly by tests. */
int host_stat(const char *path, FILINFO *fno, api_errno *err);
int host_opendir(const char *path, api_errno *err); /* >=0 descriptor, or -1 */
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
