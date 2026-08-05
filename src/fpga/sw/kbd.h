/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_KBD_H_
#define _FPGA_SW_KBD_H_

#include <stdbool.h>
#include <stdint.h>

bool kbd_set_xram(uint16_t addr);
void kbd_task(void);
void kbd_stop(void);

#endif /* _FPGA_SW_KBD_H_ */
