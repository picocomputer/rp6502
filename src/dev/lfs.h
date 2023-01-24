/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _LFS_H_
#define _LFS_H_

#include "hardware/flash.h"
#include "littlefs/lfs.h"

#define _LFS_FILE_CONFIG_NAME(name) lfs_file_config_buffer_##name
#define LFS_FILE_CONFIG(name)                             \
    uint8_t _LFS_FILE_CONFIG_NAME(name)[FLASH_PAGE_SIZE]; \
    struct lfs_file_config lfs_file_config = {            \
        .buffer = _LFS_FILE_CONFIG_NAME(name),            \
    };

void lfs_init();
extern lfs_t lfs_volume;

#endif /* _LFS_H_ */
