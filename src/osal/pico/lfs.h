/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_PICO_LFS_H_
#define _OSAL_PICO_LFS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <hardware/flash.h>
#include <littlefs/lfs.h>
#include "core/api/api.h"

extern lfs_t lfs_volume;

/* The result lfs_init got from the mount, kept because the filesystem cannot
 * report it itself. It is zero once the volume is up. */
extern int lfs_mount_error;

#define _LFS_FILE_CONFIG_NAME(name) lfs_file_config_buffer_##name
#define LFS_FILE_CONFIG(name, ...)                                    \
    __VA_ARGS__ uint8_t _LFS_FILE_CONFIG_NAME(name)[FLASH_PAGE_SIZE]; \
    __VA_ARGS__ struct lfs_file_config name = {                       \
        .buffer = _LFS_FILE_CONFIG_NAME(name),                        \
    };

void lfs_init(void);

// Returns 1 at end of file, 0 if not, or -1 on error.
int lfs_eof(lfs_t *lfs, lfs_file_t *file);

/* The format expands through a buffer of this size before a single write,
 * and anything longer is truncated. */
#define LFS_PRINTF_MAX 320
int lfs_printf(lfs_t *lfs, lfs_file_t *file, const char *format, ...);

char *lfs_gets(char *str, size_t n, lfs_t *lfs, lfs_file_t *file, int *err);

#define LFS_DRIVER DRIVER(lfs_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _OSAL_PICO_LFS_H_ */
