/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_PICO_LFS_H_
#define _OSAL_PICO_LFS_H_

/* Arm's littlefs for non-volatile storage.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <hardware/flash.h>
#include <littlefs/lfs.h>
#include "core/api/api.h"

// Our only volume is mounted here for all to use.
extern lfs_t lfs_volume;

/* What the mount came to, for the machine to say out loud: a filesystem has
 * no console of its own. Zero once the volume is up. */
extern int lfs_mount_error;

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
// Returns 1 at EOF, 0 if not, or -1 on error.
int lfs_eof(lfs_t *lfs, lfs_file_t *file);

/* Print formatted characters to the file. One write, so the format expands
 * through a buffer of this size and anything longer is truncated. */
#define LFS_PRINTF_MAX 320
int lfs_printf(lfs_t *lfs, lfs_file_t *file, const char *format, ...);

// Safe gets.
char *lfs_gets(char *str, size_t n, lfs_t *lfs, lfs_file_t *file, int *err);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Only an init, which is
 * still a bring-up: the volume has to be mounted before anything reads it. */
#define LFS_DRIVER DRIVER(lfs_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _OSAL_PICO_LFS_H_ */
