/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MOU_H_
#define _FPGA_SW_MOU_H_

#include <stdbool.h>
#include <stdint.h>

bool mou_set_xram(uint16_t word);
void mou_task(void);
void mou_stop(void);

#endif /* _FPGA_SW_MOU_H_ */
