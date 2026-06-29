/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA clock/time API (clk.c): host wall clock, timezone, locale. The clk_api_*
 * handlers and clk_get_run are declared by the firmware api/clk.h, which clk.c
 * and ria.c include; only clk_reset is emu-specific.
 */

#ifndef _EMU_CLK_H_
#define _EMU_CLK_H_

#ifdef __cplusplus
extern "C"
{
#endif

void clk_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_CLK_H_ */
