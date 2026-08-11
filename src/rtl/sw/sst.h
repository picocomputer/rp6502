/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_SST_H_
#define _FPGA_SW_SST_H_

#include <stdbool.h>

bool sst_pending(void);

void sst_task(void);

#endif
