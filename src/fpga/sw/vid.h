/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VID_H_
#define _FPGA_SW_VID_H_

/* Once per frame, publish the terminal's scanout state to the vid
 * registers: the resolved row bases, the cursor, the blink phase. */
void vid_task(void);

#endif /* _FPGA_SW_VID_H_ */
