/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_VID_H_
#define _HOST_POCKET_SW_VID_H_

#include <stdint.h>
#include <stdbool.h>

void vid_init(void);

void vid_stop(void);
void vid_task(void);
void vid_restore(void);
uint32_t vid_prog_word_get(void);

bool mode0_prog(uint16_t *xregs);


/* VID_DRIVER follows TERM_DRIVER in the driver list because vid_init
 * selects the console canvas, which calls term_set_height. */
#define VID_DRIVER DRIVER(vid_init, nul_task, vid_task, nul_run, vid_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_VID_H_ */
