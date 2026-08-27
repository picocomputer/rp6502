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

/* This machine's cpu row. cpu_init is not in it: it drives RESB low, which has
 * to happen before anything else runs, so the machine calls it before the walk
 * rather than wherever the shared order would have put it. */
#undef CPU_LIFECYCLE
#define CPU_LIFECYCLE LIFECYCLE(nul_init, cpu_run, cpu_stop, nul_break)

#endif /* _FPGA_SW_CPU_H_ */
