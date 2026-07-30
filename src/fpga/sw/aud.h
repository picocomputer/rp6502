/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_AUD_H_
#define _FPGA_SW_AUD_H_

#include <stdbool.h>
#include <stdint.h>

bool aud_psg_xreg(uint16_t word);
bool aud_opl_xreg(uint16_t word);
void aud_task(void);

#endif /* _FPGA_SW_AUD_H_ */
