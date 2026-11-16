/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_VID_H_
#define _HOST_POCKET_SW_VID_H_

#include "vga.h"

#include <stdint.h>
#include <stdbool.h>

void vid_init(void);

void vid_stop(void);
void vid_task(void);

/* The canvas 2, the code page 2, then vga.c's table. */
#define VID_SST_SIZE (2 + 2 + VGA_TABLE_SST_SIZE)
void vid_sst_save(sst_cursor_t *c, unsigned flags);
bool vid_sst_load(sst_cursor_t *c, unsigned flags);

/* VID_DRIVER follows CELLS_DRIVER in the driver list because vid_init
 * selects the console canvas, which calls cells_set_height, and because
 * its savestate load publishes the cells that CELLS_DRIVER loads. */
#define VID_DRIVER DRIVER(                                     \
    INIT(vid_init)                                             \
    TASK_FATFS(vid_task)                                       \
    STOP(vid_stop)                                             \
    SST(VID_, 2, VID_SST_SIZE, vid_sst_save, vid_sst_load))

#endif /* _HOST_POCKET_SW_VID_H_ */
