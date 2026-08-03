/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_AUD_H_
#define _FPGA_SW_AUD_H_

#include <stdbool.h>
#include <stdint.h>

void aud_init(void);
void aud_stop(void);
bool aud_psg_xreg(uint16_t word);
bool aud_opl_xreg(uint16_t word);

#endif /* _FPGA_SW_AUD_H_ */
