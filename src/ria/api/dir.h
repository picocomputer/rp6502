/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_DIR_H_
#define _RIA_API_DIR_H_

/* File and directory management
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void dir_run(void);
void dir_stop(void);

/* The API implementations
 */

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

#endif /* _RIA_API_DIR_H_ */
