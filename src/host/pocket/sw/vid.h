/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VID_H_
#define _FPGA_SW_VID_H_

#include <stdint.h>
#include <stdbool.h>

/* The terminal view over the scanout hardware: init programs the raster
 * window; task publishes the model's scanout state once per frame. */
void vid_init(void);
void vid_task(void);
/* The terminal's raster window, which is written once when the mode is
 * programmed and so is gone after a wake. */
void vid_restore(void);
/* The shadow of the window register, for the wake log. */
uint32_t vid_prog_word_get(void);

bool mode0_prog(uint16_t *xregs);


#endif /* _FPGA_SW_VID_H_ */
