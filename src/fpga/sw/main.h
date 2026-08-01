/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MAIN_H_
#define _FPGA_SW_MAIN_H_

#include <stdint.h>
#include <stdbool.h>

/* The xreg fan-outs, the emulator's shape: device 0 is the RIA-local
 * virtual xreg, device 1 the video device on this fabric. */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word);
bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word);

#endif /* _FPGA_SW_MAIN_H_ */
