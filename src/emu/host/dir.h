/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
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

/* Point the OP dispatcher's dir slots at the FatFs firmware handlers (true, over the
 * RAM disk) or the host handlers (false). Called by the drive lifecycle. */
void hostdir_ops_set(bool fat);

/* The emu's HOST dir syscall handlers (emu/host/dir.c), installed in the OP array. */
bool hostdir_api_stat(void);
bool hostdir_api_opendir(void);
bool hostdir_api_readdir(void);
bool hostdir_api_closedir(void);
bool hostdir_api_telldir(void);
bool hostdir_api_seekdir(void);
bool hostdir_api_rewinddir(void);
bool hostdir_api_unlink(void);
bool hostdir_api_rename(void);
bool hostdir_api_chmod(void);
bool hostdir_api_utime(void);
bool hostdir_api_mkdir(void);
bool hostdir_api_chdir(void);
bool hostdir_api_chdrive(void);
bool hostdir_api_getcwd(void);
bool hostdir_api_setlabel(void);
bool hostdir_api_getlabel(void);
bool hostdir_api_getfree(void);

void hostdir_stop(void); /* close open host directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_DIR_H_ */
