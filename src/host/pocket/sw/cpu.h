/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_CPU_H_
#define _FPGA_SW_CPU_H_

#include "core/cpu.h"

/* This machine's 6502 lifecycle, called from its main.c. */
void cpu_init(void);
void cpu_run(void);
void cpu_stop(void);

#endif /* _FPGA_SW_CPU_H_ */
