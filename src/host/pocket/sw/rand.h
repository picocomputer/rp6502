/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_RAND_H_
#define _FPGA_SW_RAND_H_

void rand_init(void);

/* This driver's row in a machine's driver list; see core/driver.h. */
#define RAND_DRIVER DRIVER(rand_init, nul_task, nul_task, nul_run, nul_stop, nul_break)

#endif /* _FPGA_SW_RAND_H_ */
