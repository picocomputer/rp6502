/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_DIR_H_
#define _CORE_API_DIR_H_

#include "osal/dir.h"
#include "core/sys/sst.h"

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

/* Eight entry counters of four bytes, then eight slots of an open flag and a
 * path, then the working directory. */
#define DIR_SST_SIZE (8 * 4 + 8 * (1 + API_PATH_MAX + 1) + API_PATH_MAX + 1)
void dir_sst_save(sst_cursor_t *c, unsigned flags);
bool dir_sst_load(sst_cursor_t *c, unsigned flags);

#define DIR_DRIVER DRIVER(nul_init, nul_task, nul_task, dir_run, dir_stop, nul_break, \
    nul_config, nul_config, SST(DIR_, 1, DIR_SST_SIZE, dir_sst_save, dir_sst_load))

#endif /* _CORE_API_DIR_H_ */
