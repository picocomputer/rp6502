/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VID_H_
#define _FPGA_SW_VID_H_

/* The terminal view over the scanout hardware: init programs the raster
 * window; task publishes the model's scanout state once per frame. */
void vid_init(void);
void vid_task(void);

#endif /* _FPGA_SW_VID_H_ */
