/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: addressing + mount (mscpath.c): map a 6502 path to a host path, manage
 * the mount root and the current dir/drive. The mount is set ONCE (the launch
 * dir, --fs, or a --tmpdrive throwaway) and persists the whole session, exec
 * included. "//C/..." names a Windows drive; an unprefixed path defaults to MSC0:.
 */

#ifndef _EMU_MSCPATH_H_
#define _EMU_MSCPATH_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool fs_set_cwd(const char *hostdir); /* mount MSC0: at a host dir (--fs / boot default) */
bool fs_use_tmpdrive(void);           /* --tmpdrive: MSC0: -> a fresh throwaway temp dir */
bool fs_to_host(const char *path, char *host, size_t hsz);            /* MSC0: -> host path */
size_t fs_host_to_msc(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool fs_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *fs_strip_drive(const char *path); /* path past a recognized drive prefix */
int fs_chdir(const char *path);
int fs_chdrive(const char *drive);
size_t fs_getcwd(char *out, size_t outsz); /* "MSC0:<cwd>"; returns its length (0 = did not fit) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MSCPATH_H_ */
