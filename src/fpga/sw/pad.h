/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PAD_H_
#define _FPGA_SW_PAD_H_

#include <stdbool.h>
#include <stdint.h>

bool pad_set_xram(uint16_t word);
void pad_task(void);
void pad_stop(void);

#endif /* _FPGA_SW_PAD_H_ */
