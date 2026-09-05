/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * littlefs's log macros, ahead of its own through LFS_DEFINES: its lines are
 * the lfs category, without the __FILE__ prefixes its defaults carry.
 */

#ifndef _OSAL_PICO_LFS_LOG_H_
#define _OSAL_PICO_LFS_LOG_H_

#include "core/sys/debug_log.h"

#define LFS_TRACE(...)
#define LFS_DEBUG(...) RP6502_LOG(lfs, DEBUG, __VA_ARGS__)
#define LFS_WARN(...) RP6502_LOG(lfs, WARN, __VA_ARGS__)
#define LFS_ERROR(...) RP6502_LOG(lfs, ERROR, __VA_ARGS__)

#endif /* _OSAL_PICO_LFS_LOG_H_ */
