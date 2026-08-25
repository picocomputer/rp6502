/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "core/com.h"

/* This machine's once-a-frame console service; the rest of the console is
 * core/com.h, answered by core/com/com.c. */
void com_task(void);

#endif /* _FPGA_SW_COM_H_ */
