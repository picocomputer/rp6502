/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_LFS_H_
#define _RIA_SYS_LFS_H_

/* Arm's littlefs for non-volatile storage.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <hardware/flash.h>
#include <littlefs/lfs.h>

// Our only volume is mounted here for all to use.
extern lfs_t lfs_volume;

// Use this to obtain a temporary lfs_file_config on the stack.
#define _LFS_FILE_CONFIG_NAME(name) lfs_file_config_buffer_##name
#define LFS_FILE_CONFIG(name, ...)                                    \
    __VA_ARGS__ uint8_t _LFS_FILE_CONFIG_NAME(name)[FLASH_PAGE_SIZE]; \
    __VA_ARGS__ struct lfs_file_config name = {                       \
        .buffer = _LFS_FILE_CONFIG_NAME(name),                        \
    };

/* Main events
 */

void lfs_init(void);

// Test if file position is at the end of the file.
int lfs_eof(lfs_file_t *file);

// Print formatted characters to the file.
int lfs_printf(lfs_t *lfs, lfs_file_t *file, const char *format, ...);

// Safe gets.
char *lfs_gets(char *str, int n, lfs_t *lfs, lfs_file_t *file);

#endif /* _RIA_SYS_LFS_H_ */
