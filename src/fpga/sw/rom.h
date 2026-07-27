/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_ROM_H_
#define _FPGA_SW_ROM_H_

#include <stdbool.h>
#include <stdint.h>

/* Load the .rp6502 image the platform staged, len bytes at the staging
 * window. True when the program and its reset vector are in place. */
bool rom_load_staged(uint32_t len);

#endif /* _FPGA_SW_ROM_H_ */
