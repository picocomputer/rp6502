/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_POSIX_FS_H_
#define _OSAL_POSIX_FS_H_

/* fs_std_close in fs_aio.c and fs_sync.c calls this just before the OS
 * closes desc, so what fs.c keeps for the descriptor ends before the OS can
 * hand its number to another file. */
void fs_closing(int desc);

#endif /* _OSAL_POSIX_FS_H_ */
