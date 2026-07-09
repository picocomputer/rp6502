/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_API_HOSTFS_H_
#define _EMU_API_HOSTFS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api/api.h"  /* api_errno */
#include "fatfs/ff.h" /* FILINFO */

#ifdef __cplusplus
extern "C"
{
#endif

/* The emu's native host filesystem syscall handlers, installed in the OP array. */
bool hostfs_api_stat(void);
bool hostfs_api_opendir(void);
bool hostfs_api_readdir(void);
bool hostfs_api_closedir(void);
bool hostfs_api_telldir(void);
bool hostfs_api_seekdir(void);
bool hostfs_api_rewinddir(void);
bool hostfs_api_unlink(void);
bool hostfs_api_rename(void);
bool hostfs_api_chmod(void);
bool hostfs_api_utime(void);
bool hostfs_api_mkdir(void);
bool hostfs_api_chdir(void);
bool hostfs_api_chdrive(void);
bool hostfs_api_getcwd(void);
bool hostfs_api_setlabel(void);
bool hostfs_api_getlabel(void);
bool hostfs_api_getfree(void);

void hostfs_stop(void); /* close open host directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_API_HOSTFS_H_ */
