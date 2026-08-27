/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_RAND_H_
#define _FPGA_SW_RAND_H_

void rand_init(void);

/* This driver's lifecycle row; see core/lifecycle.h. */
#define RAND_LIFECYCLE LIFECYCLE(rand_init, nul_run, nul_stop, nul_break)

#endif /* _FPGA_SW_RAND_H_ */
