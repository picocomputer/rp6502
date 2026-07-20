/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_API_CLK_H_
#define _EMU_API_CLK_H_

#include "ria/api/clk.h"

void clk_init(void); /* cold boot: adopt the host timezone/locale */
void clk_run(void);  /* program start: clear the settime offset + re-anchor the run clock */

#endif /* _EMU_API_CLK_H_ */
