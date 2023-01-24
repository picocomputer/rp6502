/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _LFS_H_
#define _LFS_H_

#include "littlefs/lfs.h"

void lfs_init();
extern lfs_t lfs_volume;

#endif /* _LFS_H_ */
