/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "core/com.h"

/* This machine's console lifecycle. What every machine's console can do is
 * core/com.h; these three are the Pocket's own, called from its main.c. */
void com_init(void);
void com_run(void);
void com_task(void);

#endif /* _FPGA_SW_COM_H_ */
