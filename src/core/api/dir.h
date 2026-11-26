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
bool dir_api_remove(void);
bool dir_api_rename(void);
bool dir_api_chmod(void);
bool dir_api_utime(void);
bool dir_api_mkdir(void);
bool dir_api_chdir(void);
bool dir_api_getcwd(void);
bool dir_api_setlabel(void);
bool dir_api_getlabel(void);
bool dir_api_getfree(void);

extern const sst_table_t dir_sst;

/* A failed savestate load closes every directory either machine had open
 * and zeroes the entry counts, as a program start does, so no handle is held
 * while the machine waits for one. */
#define DIR_DRIVER DRIVER(                                  \
    RUN(dir_run)                                            \
    STOP(dir_stop)                                          \
    RESET(dir_run)                                          \
    SST("DIR_", dir))

#endif /* _CORE_API_DIR_H_ */
