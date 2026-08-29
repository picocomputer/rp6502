/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_CPU_H_
#define _FPGA_SW_CPU_H_

#include "core/cpu.h"

/* This machine's 6502 driver row. */
void cpu_init(void);
void cpu_run(void);
void cpu_stop(void);

/* This driver's row in a machine's driver list; see core/driver.h. A row lives with the
 * implementation, not the contract, which is why no #undef is needed: this
 * is the only cpu row a Pocket translation unit can see. */
#define CPU_DRIVER DRIVER(cpu_init, nul_task, nul_task, cpu_run, cpu_stop, nul_break)

#endif /* _FPGA_SW_CPU_H_ */
