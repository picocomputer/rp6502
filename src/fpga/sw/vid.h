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

/* The mode 0 program, this machine's terminal view. */
bool vid_mode0_prog(uint16_t *xregs);

/* The code page, which is the glyphs: setting one rebuilds font.c's
 * tables and stores them to the video device. */
void vid_set_code_page(uint16_t cp);
uint16_t vid_get_code_page(void);

#endif /* _FPGA_SW_VID_H_ */
