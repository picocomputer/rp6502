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
void vid_run(void);

extern const sst_table_t vid_sst;

/* VID_DRIVER follows CELLS_DRIVER in the driver list because vid_init
 * selects the console canvas, which calls cells_set_height. */
#define VID_DRIVER DRIVER(                                     \
    INIT(vid_init)                                             \
    TASK_FATFS(vid_task)                                       \
    RUN(vid_run)                                               \
    STOP(vid_stop)                                             \
    RESET(vid_stop)                                            \
    SST("VID_", vid))

#endif /* _HOST_POCKET_SW_VID_H_ */
