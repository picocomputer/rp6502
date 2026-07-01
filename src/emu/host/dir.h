/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: on the native host filesystem: path addressing plus the directory +
 * file-metadata syscall handlers (host_dir_api_*, in emu/host/dir.c) — stat, the
 * opendir/readdir family, free space, unlink/rename/mkdir/chmod/utime/label, the
 * cwd — each doing the POSIX work and pushing the FatFs FILINFO the 6502 reads. The
 * OP dispatcher installs them for the default host drive and swaps in the real
 * firmware dir_api_* (ria/api/dir.c) on --tmpdrive. "MSC0:" maps straight onto the
 * OS filesystem: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a Windows drive.
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

/* The emu's HOST dir syscall handlers (emu/host/dir.c), installed in the OP array. */
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

void host_dir_stop(void); /* close open host directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_DIR_H_ */
