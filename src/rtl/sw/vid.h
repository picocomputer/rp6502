/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VID_H_
#define _FPGA_SW_VID_H_

#include <stdint.h>
#include <stdbool.h>

void vid_init(void);
void vid_task(void);
void vid_restore(void);
uint32_t vid_prog_word_get(void);

bool vid_mode0_prog(uint16_t *xregs);

#endif
