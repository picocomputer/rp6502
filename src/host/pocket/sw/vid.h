/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_VID_H_
#define _HOST_POCKET_SW_VID_H_

#include <stdint.h>
#include <stdbool.h>

/* The terminal view over the scanout hardware: init programs the raster
 * window; task publishes the model's scanout state once per frame. */
void vid_init(void);

/* Put the display back the way a fresh boot finds it. */
void vid_stop(void);
void vid_task(void);
/* The terminal's raster window, which is written once when the mode is
 * programmed and so is gone after a wake. */
void vid_restore(void);
/* The shadow of the window register, for the wake log. */
uint32_t vid_prog_word_get(void);

bool mode0_prog(uint16_t *xregs);


/* This driver's row in a machine's driver list; see core/sys/driver.h. After TERM, because
 * vid_init selects a canvas and that calls term_set_height. Its stop defers
 * the display restore to vid_task, so this row does not also have to be
 * first for the sake of being last. */
#define VID_DRIVER DRIVER(vid_init, nul_task, vid_task, nul_run, vid_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_VID_H_ */
