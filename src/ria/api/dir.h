/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_DIR_H_
#define _RIA_API_DIR_H_

/* Implements dirent.h for 6502
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void dir_run(void);
void dir_stop(void);

/* The API implementation for dirent.h
 */

bool dir_api_opendir(void);
bool dir_api_readdir(void);
bool dir_api_closedir(void);
bool dir_api_telldir(void);
bool dir_api_seekdir(void);
bool dir_api_rewinddir(void);
bool dir_api_unlink(void);
bool dir_api_rename(void);
bool dir_api_stat(void);
bool dir_api_chmod(void);

#endif /* _RIA_API_DIR_H_ */
